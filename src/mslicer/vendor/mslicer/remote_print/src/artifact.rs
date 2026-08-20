//! Source-pinned, one-shot HTTP serving for an already encoded resin artifact.

use std::{
    fmt,
    io::{Read, Write},
    net::{Ipv4Addr, SocketAddr, TcpListener, TcpStream},
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
    time::{Duration, Instant},
};

const MAX_REQUEST_BYTES: usize = 4096;
const IO_POLL: Duration = Duration::from_millis(50);
const WRITE_CHUNK: usize = 64 * 1024;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ArtifactError {
    InvalidArtifact,
    InvalidFilename,
    Timeout,
    Cancelled,
    Io(String),
}

impl fmt::Display for ArtifactError {
    fn fmt(&self, out: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidArtifact => {
                out.write_str("resin upload artifact is empty or exceeds the SDCP size limit")
            }
            Self::InvalidFilename => {
                out.write_str("resin upload filename must be a safe CTB or GOO filename")
            }
            Self::Timeout => {
                out.write_str("printer did not pull the resin artifact before the deadline")
            }
            Self::Cancelled => out.write_str("resin artifact transfer was cancelled"),
            Self::Io(message) => write!(out, "resin artifact transfer I/O error: {message}"),
        }
    }
}

impl std::error::Error for ArtifactError {}

pub struct ArtifactServer {
    listener: TcpListener,
    port: u16,
    expected_source: Ipv4Addr,
    path: String,
    filename: String,
    artifact: Arc<[u8]>,
}

impl ArtifactServer {
    pub fn bind(
        local_ip: Ipv4Addr,
        expected_source: Ipv4Addr,
        filename: &str,
        artifact: Arc<[u8]>,
    ) -> Result<Self, ArtifactError> {
        if artifact.is_empty() || artifact.len() > u32::MAX as usize {
            return Err(ArtifactError::InvalidArtifact);
        }
        if !valid_filename(filename) {
            return Err(ArtifactError::InvalidFilename);
        }
        let listener = TcpListener::bind((local_ip, 0)).map_err(io_error)?;
        let port = listener.local_addr().map_err(io_error)?.port();
        listener.set_nonblocking(true).map_err(io_error)?;
        Ok(Self {
            listener,
            port,
            expected_source,
            path: format!("/{:032x}/{filename}", rand::random::<u128>()),
            filename: filename.to_owned(),
            artifact,
        })
    }

    pub fn port(&self) -> u16 {
        self.port
    }

    pub fn path(&self) -> &str {
        &self.path
    }

    pub fn filename(&self) -> &str {
        &self.filename
    }

    pub fn size(&self) -> u32 {
        self.artifact.len() as u32
    }

    pub fn md5(&self) -> String {
        format!("{:x}", md5::compute(&self.artifact))
    }

    pub fn serve(
        self,
        timeout: Duration,
        cancelled: &AtomicBool,
        mut progress: impl FnMut(u64, u64),
    ) -> Result<(), ArtifactError> {
        self.serve_with_cancel(timeout, || cancelled.load(Ordering::Acquire), &mut progress)
    }

    pub fn serve_with_cancel(
        self,
        timeout: Duration,
        cancelled: impl Fn() -> bool,
        mut progress: impl FnMut(u64, u64),
    ) -> Result<(), ArtifactError> {
        if timeout.is_zero() {
            return Err(ArtifactError::Timeout);
        }
        let started = Instant::now();
        loop {
            check_time(started, timeout, &cancelled)?;
            match self.listener.accept() {
                Ok((mut stream, source)) => {
                    let allowed = matches!(source, SocketAddr::V4(address) if *address.ip() == self.expected_source);
                    if !allowed {
                        reject(&mut stream, 403, "Forbidden")?;
                        continue;
                    }
                    match self.handle_request(
                        &mut stream,
                        started,
                        timeout,
                        &cancelled,
                        &mut progress,
                    )? {
                        RequestOutcome::Served => return Ok(()),
                        RequestOutcome::Continue => continue,
                    }
                }
                Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {
                    std::thread::sleep(IO_POLL.min(remaining(started, timeout)?));
                }
                Err(error) => return Err(io_error(error)),
            }
        }
    }

    fn handle_request(
        &self,
        stream: &mut TcpStream,
        started: Instant,
        timeout: Duration,
        cancelled: &impl Fn() -> bool,
        progress: &mut impl FnMut(u64, u64),
    ) -> Result<RequestOutcome, ArtifactError> {
        stream.set_read_timeout(Some(IO_POLL)).map_err(io_error)?;
        stream.set_write_timeout(Some(IO_POLL)).map_err(io_error)?;
        let mut request = Vec::with_capacity(512);
        let mut chunk = [0_u8; 512];
        while !request.windows(4).any(|bytes| bytes == b"\r\n\r\n") {
            check_time(started, timeout, cancelled)?;
            match stream.read(&mut chunk) {
                Ok(0) => return Ok(RequestOutcome::Continue),
                Ok(length) => {
                    if request.len() + length > MAX_REQUEST_BYTES {
                        reject(stream, 431, "Request Header Fields Too Large")?;
                        return Ok(RequestOutcome::Continue);
                    }
                    request.extend_from_slice(&chunk[..length]);
                }
                Err(error)
                    if matches!(
                        error.kind(),
                        std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
                    ) => {}
                Err(error) => return Err(io_error(error)),
            }
        }

        let first_line = request
            .split(|byte| *byte == b'\n')
            .next()
            .unwrap_or_default();
        let first_line = std::str::from_utf8(first_line)
            .unwrap_or_default()
            .trim_end_matches('\r');
        let mut parts = first_line.split(' ');
        let method = parts.next().unwrap_or_default();
        let path = parts.next().unwrap_or_default();
        let version = parts.next().unwrap_or_default();
        if parts.next().is_some() || version != "HTTP/1.1" || path != self.path {
            reject(stream, 404, "Not Found")?;
            return Ok(RequestOutcome::Continue);
        }
        if method == "HEAD" {
            write_headers(stream, self.artifact.len())?;
            return Ok(RequestOutcome::Continue);
        }
        if method != "GET" {
            reject(stream, 405, "Method Not Allowed")?;
            return Ok(RequestOutcome::Continue);
        }

        write_headers(stream, self.artifact.len())?;
        let total = self.artifact.len() as u64;
        for (index, bytes) in self.artifact.chunks(WRITE_CHUNK).enumerate() {
            write_all_bounded(stream, bytes, started, timeout, cancelled)?;
            progress(((index * WRITE_CHUNK) + bytes.len()) as u64, total);
        }
        stream.flush().map_err(io_error)?;
        Ok(RequestOutcome::Served)
    }
}

enum RequestOutcome {
    Served,
    Continue,
}

fn valid_filename(filename: &str) -> bool {
    if filename.is_empty() || filename.len() > 128 || filename.starts_with('.') {
        return false;
    }
    if !filename
        .bytes()
        .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b'-'))
    {
        return false;
    }
    filename.rsplit_once('.').is_some_and(|(_, extension)| {
        extension.eq_ignore_ascii_case("ctb") || extension.eq_ignore_ascii_case("goo")
    })
}

fn write_headers(stream: &mut TcpStream, size: usize) -> Result<(), ArtifactError> {
    write!(stream, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: {size}\r\nConnection: close\r\n\r\n").map_err(io_error)
}

fn reject(stream: &mut TcpStream, code: u16, reason: &str) -> Result<(), ArtifactError> {
    write!(
        stream,
        "HTTP/1.1 {code} {reason}\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
    )
    .map_err(io_error)
}

fn write_all_bounded(
    stream: &mut TcpStream,
    mut bytes: &[u8],
    started: Instant,
    timeout: Duration,
    cancelled: &impl Fn() -> bool,
) -> Result<(), ArtifactError> {
    while !bytes.is_empty() {
        check_time(started, timeout, cancelled)?;
        match stream.write(bytes) {
            Ok(0) => {
                return Err(ArtifactError::Io(
                    "printer closed the artifact connection".into(),
                ));
            }
            Ok(length) => bytes = &bytes[length..],
            Err(error)
                if matches!(
                    error.kind(),
                    std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
                ) => {}
            Err(error) => return Err(io_error(error)),
        }
    }
    Ok(())
}

fn check_time(
    started: Instant,
    timeout: Duration,
    cancelled: &impl Fn() -> bool,
) -> Result<(), ArtifactError> {
    if cancelled() {
        Err(ArtifactError::Cancelled)
    } else if started.elapsed() >= timeout {
        Err(ArtifactError::Timeout)
    } else {
        Ok(())
    }
}

fn remaining(started: Instant, timeout: Duration) -> Result<Duration, ArtifactError> {
    timeout
        .checked_sub(started.elapsed())
        .ok_or(ArtifactError::Timeout)
}

fn io_error(error: std::io::Error) -> ArtifactError {
    ArtifactError::Io(error.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{sync::Mutex, thread};

    fn request(address: SocketAddr, request: &str) -> Vec<u8> {
        let mut stream = TcpStream::connect(address).unwrap();
        stream.write_all(request.as_bytes()).unwrap();
        let mut response = Vec::new();
        stream.read_to_end(&mut response).unwrap();
        response
    }

    #[test]
    fn serves_exact_artifact_after_optional_head() {
        let bytes: Arc<[u8]> = Arc::from(vec![0_u8, 1, 2, 3, 255]);
        let server = ArtifactServer::bind(
            Ipv4Addr::LOCALHOST,
            Ipv4Addr::LOCALHOST,
            "part.goo",
            bytes.clone(),
        )
        .unwrap();
        let address = SocketAddr::from((Ipv4Addr::LOCALHOST, server.port()));
        let path = server.path().to_owned();
        assert_eq!(server.size(), 5);
        assert_eq!(server.md5(), format!("{:x}", md5::compute(&bytes)));
        let progress = Arc::new(Mutex::new(Vec::new()));
        let worker_progress = progress.clone();
        let worker = thread::spawn(move || {
            server.serve(
                Duration::from_secs(2),
                &AtomicBool::new(false),
                |done, total| worker_progress.lock().unwrap().push((done, total)),
            )
        });

        let head = request(
            address,
            &format!("HEAD {path} HTTP/1.1\r\nHost: test\r\n\r\n"),
        );
        assert!(head.starts_with(b"HTTP/1.1 200 OK"));
        assert!(!head.ends_with(&bytes));
        let response = request(
            address,
            &format!("GET {path} HTTP/1.1\r\nHost: test\r\n\r\n"),
        );
        assert!(response.starts_with(b"HTTP/1.1 200 OK"));
        assert!(response.ends_with(&bytes));
        worker.join().unwrap().unwrap();
        assert_eq!(*progress.lock().unwrap(), vec![(5, 5)]);
        assert!(TcpStream::connect(address).is_err());
    }

    #[test]
    fn rejects_wrong_path_without_exposing_artifact() {
        let bytes: Arc<[u8]> = Arc::from(b"secret-artifact".as_slice());
        let server = ArtifactServer::bind(
            Ipv4Addr::LOCALHOST,
            Ipv4Addr::LOCALHOST,
            "part.ctb",
            bytes.clone(),
        )
        .unwrap();
        let address = SocketAddr::from((Ipv4Addr::LOCALHOST, server.port()));
        let path = server.path().to_owned();
        let worker = thread::spawn(move || {
            server.serve(Duration::from_secs(2), &AtomicBool::new(false), |_, _| {})
        });
        let rejected = request(address, "GET /part.ctb HTTP/1.1\r\nHost: test\r\n\r\n");
        assert!(rejected.starts_with(b"HTTP/1.1 404 Not Found"));
        assert!(!rejected.ends_with(&bytes));
        let accepted = request(
            address,
            &format!("GET {path} HTTP/1.1\r\nHost: test\r\n\r\n"),
        );
        assert!(accepted.ends_with(&bytes));
        worker.join().unwrap().unwrap();
    }

    #[test]
    fn validates_filename_and_obeys_cancellation() {
        let bytes: Arc<[u8]> = Arc::from(b"x".as_slice());
        assert!(matches!(
            ArtifactServer::bind(
                Ipv4Addr::LOCALHOST,
                Ipv4Addr::LOCALHOST,
                "../part.goo",
                bytes.clone()
            ),
            Err(ArtifactError::InvalidFilename)
        ));
        assert!(matches!(
            ArtifactServer::bind(
                Ipv4Addr::LOCALHOST,
                Ipv4Addr::LOCALHOST,
                "part.svg",
                bytes.clone()
            ),
            Err(ArtifactError::InvalidFilename)
        ));
        let server =
            ArtifactServer::bind(Ipv4Addr::LOCALHOST, Ipv4Addr::LOCALHOST, "part.goo", bytes)
                .unwrap();
        assert_eq!(
            server.serve(Duration::from_secs(1), &AtomicBool::new(true), |_, _| {}),
            Err(ArtifactError::Cancelled)
        );
    }

    #[test]
    fn idle_server_times_out_and_closes_listener() {
        let server = ArtifactServer::bind(
            Ipv4Addr::LOCALHOST,
            Ipv4Addr::LOCALHOST,
            "part.goo",
            Arc::from(b"x".as_slice()),
        )
        .unwrap();
        let address = SocketAddr::from((Ipv4Addr::LOCALHOST, server.port()));
        assert_eq!(
            server.serve(
                Duration::from_millis(20),
                &AtomicBool::new(false),
                |_, _| {}
            ),
            Err(ArtifactError::Timeout)
        );
        assert!(TcpStream::connect(address).is_err());
    }
}
