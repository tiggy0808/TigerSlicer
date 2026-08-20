//! Explicit, owned SDCP upload session. No listener or worker survives the call.

use std::{
    fmt,
    io::{Read, Write},
    net::{Ipv4Addr, SocketAddr, SocketAddrV4, TcpListener, TcpStream, UdpSocket},
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
    time::{Duration, Instant},
};

use crate::{
    Resolution, Response,
    artifact::{ArtifactError, ArtifactServer},
    commands::{Command, CommandTrait, UploadFile},
    mqtt::packets::{
        MAX_PACKET_BYTES, Packet,
        connect::ConnectPacket,
        connect_ack::{ConnectAckFlags, ConnectAckPacket, ConnectReturnCode},
        publish::{PublishFlags, PublishPacket},
        publish_ack::PublishAckPacket,
        subscribe::SubscribePacket,
        subscribe_ack::{SubscribeAckPacket, SubscribeReturnCode},
    },
    probe::{PrinterIdentity, ProbeError, probe_printer_inner},
    status::{FileTransferStatus, StatusData},
};

const IO_POLL: Duration = Duration::from_millis(50);

pub struct UploadRequest {
    pub target: SocketAddrV4,
    pub resolution: Resolution,
    pub filename: String,
    pub artifact: Arc<[u8]>,
    pub probe_timeout: Duration,
    pub connect_timeout: Duration,
    pub transfer_timeout: Duration,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct UploadReceipt {
    pub printer: PrinterIdentity,
    pub filename: String,
    pub bytes: u32,
    pub md5: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SessionError {
    Probe(ProbeError),
    Artifact(ArtifactError),
    Timeout(&'static str),
    Cancelled,
    Io(String),
    Protocol(String),
    Transfer(String),
}

impl fmt::Display for SessionError {
    fn fmt(&self, out: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Probe(error) => write!(out, "{error}"),
            Self::Artifact(error) => write!(out, "{error}"),
            Self::Timeout(stage) => write!(out, "SDCP {stage} timed out"),
            Self::Cancelled => out.write_str("SDCP upload was cancelled"),
            Self::Io(error) => write!(out, "SDCP network error: {error}"),
            Self::Protocol(error) => write!(out, "SDCP protocol error: {error}"),
            Self::Transfer(error) => write!(out, "SDCP transfer failed: {error}"),
        }
    }
}

impl std::error::Error for SessionError {}

pub fn upload_artifact(
    request: UploadRequest,
    cancelled: &AtomicBool,
    progress: impl Fn(u64, u64) + Send + Sync,
) -> Result<UploadReceipt, SessionError> {
    upload_artifact_with_cancel(request, || cancelled.load(Ordering::Acquire), progress)
}

pub fn upload_artifact_with_cancel(
    request: UploadRequest,
    cancelled: impl Fn() -> bool + Sync,
    progress: impl Fn(u64, u64) + Send + Sync,
) -> Result<UploadReceipt, SessionError> {
    upload_artifact_inner(request, cancelled, progress, false)
}

fn upload_artifact_inner(
    request: UploadRequest,
    cancelled: impl Fn() -> bool + Sync,
    progress: impl Fn(u64, u64) + Send + Sync,
    allow_loopback: bool,
) -> Result<UploadReceipt, SessionError> {
    validate_timeouts(&request)?;
    let printer = probe_printer_inner(
        request.target,
        request.resolution.clone(),
        request.probe_timeout,
        || cancelled(),
        allow_loopback,
    )
    .map_err(SessionError::Probe)?;
    check_cancelled(&cancelled)?;

    let local_ip = routed_local_ip(request.target)?;
    let mqtt = TcpListener::bind((local_ip, 0)).map_err(io_error)?;
    mqtt.set_nonblocking(true).map_err(io_error)?;
    let mqtt_port = mqtt.local_addr().map_err(io_error)?.port();
    let artifact_server = ArtifactServer::bind(
        local_ip,
        *request.target.ip(),
        &request.filename,
        request.artifact.clone(),
    )
    .map_err(SessionError::Artifact)?;
    let upload = UploadFile::with_path(
        request.filename.clone(),
        artifact_server.path().to_owned(),
        artifact_server.port(),
        &request.artifact,
    )
    .map_err(|error| SessionError::Protocol(error.to_string()))?;
    let md5 = upload.md5.clone();
    let bytes = upload.file_size;

    let callback = UdpSocket::bind((local_ip, 0)).map_err(io_error)?;
    callback.connect(request.target).map_err(io_error)?;
    callback
        .send(format!("M66666 {mqtt_port}").as_bytes())
        .map_err(io_error)?;

    let connect_started = Instant::now();
    let mut stream = accept_printer(
        &mqtt,
        *request.target.ip(),
        connect_started,
        request.connect_timeout,
        &cancelled,
    )?;
    stream.set_read_timeout(Some(IO_POLL)).map_err(io_error)?;
    stream.set_write_timeout(Some(IO_POLL)).map_err(io_error)?;
    mqtt_handshake(
        &mut stream,
        &printer.mainboard_id,
        connect_started,
        request.connect_timeout,
        &cancelled,
    )?;

    let command = Response {
        id: printer.mainboard_id.clone(),
        data: Command::new(UploadFile::CMD, upload, printer.mainboard_id.clone()),
    };
    let payload =
        serde_json::to_vec(&command).map_err(|error| SessionError::Protocol(error.to_string()))?;
    let packet = PublishPacket {
        flags: PublishFlags::QOS1,
        topic: format!("/sdcp/request/{}", printer.mainboard_id),
        packet_id: Some(1),
        data: payload,
    }
    .to_packet()
    .map_err(|error| SessionError::Protocol(error.to_string()))?;
    write_packet(
        &mut stream,
        &packet,
        connect_started,
        request.connect_timeout,
        &cancelled,
    )?;

    let local_stop = AtomicBool::new(false);
    let transfer_result = std::thread::scope(|scope| {
        let server_cancelled = || cancelled() || local_stop.load(Ordering::Acquire);
        let server = scope.spawn(move || {
            artifact_server.serve_with_cancel(
                request.transfer_timeout,
                server_cancelled,
                |done, total| progress(done, total),
            )
        });
        let mqtt_result = wait_for_completion(
            &mut stream,
            &printer.mainboard_id,
            &request.filename,
            bytes,
            Instant::now(),
            request.transfer_timeout,
            &cancelled,
        );
        if mqtt_result.is_err() {
            local_stop.store(true, Ordering::Release);
        }
        let server_result = server
            .join()
            .map_err(|_| SessionError::Protocol("artifact worker panicked".into()))?;
        mqtt_result?;
        server_result.map_err(SessionError::Artifact)
    });
    transfer_result?;

    Ok(UploadReceipt {
        printer,
        filename: request.filename,
        bytes,
        md5,
    })
}

fn validate_timeouts(request: &UploadRequest) -> Result<(), SessionError> {
    for (value, stage) in [
        (request.probe_timeout, "probe"),
        (request.connect_timeout, "connect-back"),
        (request.transfer_timeout, "transfer"),
    ] {
        if value.is_zero() || value > Duration::from_secs(600) {
            return Err(SessionError::Protocol(format!(
                "{stage} timeout is outside 1 ms to 10 minutes"
            )));
        }
    }
    Ok(())
}

fn routed_local_ip(target: SocketAddrV4) -> Result<Ipv4Addr, SessionError> {
    let route = UdpSocket::bind((Ipv4Addr::UNSPECIFIED, 0)).map_err(io_error)?;
    route.connect(target).map_err(io_error)?;
    match route.local_addr().map_err(io_error)? {
        SocketAddr::V4(address) => Ok(*address.ip()),
        SocketAddr::V6(_) => Err(SessionError::Protocol(
            "IPv6 route selected for IPv4 printer".into(),
        )),
    }
}

fn accept_printer(
    listener: &TcpListener,
    expected_ip: Ipv4Addr,
    started: Instant,
    timeout: Duration,
    cancelled: &impl Fn() -> bool,
) -> Result<TcpStream, SessionError> {
    loop {
        check_deadline(started, timeout, "connect-back", cancelled)?;
        match listener.accept() {
            Ok((stream, SocketAddr::V4(source))) if *source.ip() == expected_ip => {
                return Ok(stream);
            }
            Ok((_stream, _)) => continue,
            Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {
                std::thread::sleep(IO_POLL.min(remaining(started, timeout, "connect-back")?))
            }
            Err(error) => return Err(io_error(error)),
        }
    }
}

fn mqtt_handshake(
    stream: &mut TcpStream,
    board_id: &str,
    started: Instant,
    timeout: Duration,
    cancelled: &impl Fn() -> bool,
) -> Result<(), SessionError> {
    let connect = read_packet(stream, started, timeout, "connect-back", cancelled)?;
    let connect = ConnectPacket::from_packet(&connect).map_err(protocol)?;
    if connect.protocol_name != "MQTT"
        || connect.protocol_level != 4
        || connect.client_id.is_empty()
    {
        return Err(SessionError::Protocol(
            "printer sent an unsupported MQTT CONNECT".into(),
        ));
    }
    write_packet(
        stream,
        &ConnectAckPacket {
            flags: ConnectAckFlags::empty(),
            return_code: ConnectReturnCode::Accepted,
        }
        .to_packet(),
        started,
        timeout,
        cancelled,
    )?;

    let subscribe = read_packet(stream, started, timeout, "connect-back", cancelled)?;
    let subscribe = SubscribePacket::from_packet(&subscribe).map_err(protocol)?;
    let expected = format!("/sdcp/request/{board_id}");
    if subscribe.filters.len() != 1 || subscribe.filters[0].0 != expected {
        return Err(SessionError::Protocol(
            "printer subscribed to an unexpected SDCP identity/topic".into(),
        ));
    }
    write_packet(
        stream,
        &SubscribeAckPacket {
            packet_id: subscribe.packet_id,
            return_codes: vec![SubscribeReturnCode::Success(subscribe.filters[0].1)],
        }
        .to_packet(),
        started,
        timeout,
        cancelled,
    )
}

fn wait_for_completion(
    stream: &mut TcpStream,
    board_id: &str,
    filename: &str,
    size: u32,
    started: Instant,
    timeout: Duration,
    cancelled: &impl Fn() -> bool,
) -> Result<(), SessionError> {
    loop {
        let packet = read_packet(stream, started, timeout, "transfer", cancelled)?;
        if packet.packet_type == PublishAckPacket::PACKET_TYPE {
            PublishAckPacket::from_packet(&packet).map_err(protocol)?;
            continue;
        }
        if packet.packet_type != PublishPacket::PACKET_TYPE {
            return Err(SessionError::Protocol(
                "unexpected MQTT packet during transfer".into(),
            ));
        }
        let publish = PublishPacket::from_packet(&packet).map_err(protocol)?;
        if let Some(packet_id) = publish.packet_id {
            write_packet(
                stream,
                &PublishAckPacket { packet_id }.to_packet(),
                started,
                timeout,
                cancelled,
            )?;
        }
        let expected_topic = format!("/sdcp/status/{board_id}");
        if publish.topic != expected_topic {
            return Err(SessionError::Protocol(
                "printer published an unexpected SDCP topic".into(),
            ));
        }
        let status: Response<StatusData> =
            serde_json::from_slice(&publish.data).map_err(|error| {
                SessionError::Protocol(format!("malformed printer status: {error}"))
            })?;
        if status.id != board_id || status.data.mainboard_id != board_id {
            return Err(SessionError::Protocol(
                "printer status identity changed during transfer".into(),
            ));
        }
        let transfer = status.data.status.file_transfer_info;
        if transfer.filename != filename && transfer.status != FileTransferStatus::None {
            return Err(SessionError::Transfer(
                "printer reported a different filename".into(),
            ));
        }
        match transfer.status {
            FileTransferStatus::None => continue,
            FileTransferStatus::Error => {
                return Err(SessionError::Transfer(
                    "printer reported a file-transfer error".into(),
                ));
            }
            FileTransferStatus::Done => {
                if transfer.file_total_size != size
                    || transfer.download_offset != size
                    || transfer.check_offset != size
                {
                    return Err(SessionError::Transfer(
                        "printer completion size or offsets do not match the artifact".into(),
                    ));
                }
                return Ok(());
            }
        }
    }
}

fn read_packet(
    stream: &mut TcpStream,
    started: Instant,
    timeout: Duration,
    stage: &'static str,
    cancelled: &impl Fn() -> bool,
) -> Result<Packet, SessionError> {
    let mut fixed = [0_u8; 1];
    read_exact(stream, &mut fixed, started, timeout, stage, cancelled)?;
    let mut length = 0_usize;
    let mut multiplier = 1_usize;
    for position in 0..4 {
        let mut byte = [0_u8; 1];
        read_exact(stream, &mut byte, started, timeout, stage, cancelled)?;
        length = length
            .checked_add(((byte[0] & 0x7f) as usize) * multiplier)
            .ok_or_else(|| SessionError::Protocol("MQTT length overflow".into()))?;
        if length > MAX_PACKET_BYTES {
            return Err(SessionError::Protocol(
                "MQTT packet exceeds safety limit".into(),
            ));
        }
        if byte[0] & 0x80 == 0 {
            let mut data = vec![0_u8; length];
            read_exact(stream, &mut data, started, timeout, stage, cancelled)?;
            return Ok(Packet {
                packet_type: fixed[0] >> 4,
                flags: fixed[0] & 0x0f,
                remaining_length: length as u32,
                remaining_bytes: data,
            });
        }
        if position == 3 {
            return Err(SessionError::Protocol(
                "invalid MQTT remaining length".into(),
            ));
        }
        multiplier *= 128;
    }
    Err(SessionError::Protocol("invalid MQTT packet".into()))
}

fn read_exact(
    stream: &mut TcpStream,
    mut output: &mut [u8],
    started: Instant,
    timeout: Duration,
    stage: &'static str,
    cancelled: &impl Fn() -> bool,
) -> Result<(), SessionError> {
    while !output.is_empty() {
        check_deadline(started, timeout, stage, cancelled)?;
        match stream.read(output) {
            Ok(0) => {
                return Err(SessionError::Protocol(
                    "printer closed the MQTT connection".into(),
                ));
            }
            Ok(length) => output = &mut output[length..],
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

fn write_packet(
    stream: &mut TcpStream,
    packet: &Packet,
    started: Instant,
    timeout: Duration,
    cancelled: &impl Fn() -> bool,
) -> Result<(), SessionError> {
    let mut encoded = Vec::new();
    packet.write(&mut encoded).map_err(protocol)?;
    let mut remaining = encoded.as_slice();
    while !remaining.is_empty() {
        check_deadline(started, timeout, "MQTT write", cancelled)?;
        match stream.write(remaining) {
            Ok(0) => {
                return Err(SessionError::Protocol(
                    "printer closed the MQTT connection".into(),
                ));
            }
            Ok(length) => remaining = &remaining[length..],
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

fn check_deadline(
    started: Instant,
    timeout: Duration,
    stage: &'static str,
    cancelled: &impl Fn() -> bool,
) -> Result<(), SessionError> {
    check_cancelled(cancelled)?;
    if started.elapsed() >= timeout {
        Err(SessionError::Timeout(stage))
    } else {
        Ok(())
    }
}
fn check_cancelled(cancelled: &impl Fn() -> bool) -> Result<(), SessionError> {
    if cancelled() {
        Err(SessionError::Cancelled)
    } else {
        Ok(())
    }
}
fn remaining(
    started: Instant,
    timeout: Duration,
    stage: &'static str,
) -> Result<Duration, SessionError> {
    timeout
        .checked_sub(started.elapsed())
        .ok_or(SessionError::Timeout(stage))
}
fn io_error(error: std::io::Error) -> SessionError {
    SessionError::Io(error.to_string())
}
fn protocol(error: anyhow::Error) -> SessionError {
    SessionError::Protocol(error.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{sync::Mutex, thread};

    fn mqtt_connect() -> Packet {
        let client = b"mock-printer";
        let mut data = vec![
            0,
            4,
            b'M',
            b'Q',
            b'T',
            b'T',
            4,
            2,
            0,
            30,
            0,
            client.len() as u8,
        ];
        data.extend_from_slice(client);
        Packet {
            packet_type: ConnectPacket::PACKET_TYPE,
            flags: 0,
            remaining_length: data.len() as u32,
            remaining_bytes: data,
        }
    }

    fn mqtt_subscribe(board: &str) -> Packet {
        let topic = format!("/sdcp/request/{board}");
        let mut data = vec![0, 7, (topic.len() >> 8) as u8, topic.len() as u8];
        data.extend_from_slice(topic.as_bytes());
        data.push(1);
        Packet {
            packet_type: SubscribePacket::PACKET_TYPE,
            flags: 2,
            remaining_length: data.len() as u32,
            remaining_bytes: data,
        }
    }

    fn full_status(ip: Ipv4Addr, board: &str) -> Vec<u8> {
        format!(r#"{{"Id":"{board}","Data":{{"Attributes":{{"Name":"Mock","MachineName":"Mock Resin","ProtocolVersion":"V3.0.0","FirmwareVersion":"1.0","Resolution":"3840x2400","MainboardIP":"{ip}","MainboardID":"{board}","SDCPStatus":0,"LocalSDCPAddress":"","SDCPAddress":"","Capabilities":["FILE_TRANSFER","PRINT_CONTROL"]}},"Status":{{"CurrentStatus":0,"PreviousStatus":0,"PrintInfo":{{"Status":0,"CurrentLayer":0,"TotalLayer":0,"CurrentTicks":0,"TotalTicks":0,"ErrorNumber":0,"Filename":""}},"FileTransferInfo":{{"Status":0,"DownloadOffset":0,"CheckOffset":0,"FileTotalSize":0,"Filename":""}}}}}}}}"#).into_bytes()
    }

    fn transfer_done(board: &str, filename: &str, size: u32) -> Vec<u8> {
        format!(r#"{{"Id":"{board}","Data":{{"Status":{{"CurrentStatus":0,"PreviousStatus":2,"PrintInfo":{{"Status":0,"CurrentLayer":0,"TotalLayer":0,"CurrentTicks":0,"TotalTicks":0,"ErrorNumber":0,"Filename":""}},"FileTransferInfo":{{"Status":2,"DownloadOffset":{size},"CheckOffset":{size},"FileTotalSize":{size},"Filename":"{filename}"}}}},"MainboardID":"{board}","TimeStamp":1}}}}"#).into_bytes()
    }

    #[test]
    fn uploads_exact_bytes_through_owned_mock_session() {
        const BOARD: &str = "board-1";
        let udp = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).unwrap();
        let target = match udp.local_addr().unwrap() {
            SocketAddr::V4(address) => address,
            _ => unreachable!(),
        };
        let artifact: Arc<[u8]> = Arc::from(b"exact-goo-artifact".as_slice());
        let expected = artifact.clone();
        let pulled = Arc::new(Mutex::new(Vec::new()));
        let worker_pulled = pulled.clone();
        let mock = thread::spawn(move || {
            let mut request = [0_u8; 64];
            let (length, source) = udp.recv_from(&mut request).unwrap();
            assert_eq!(&request[..length], b"M99999");
            udp.send_to(&full_status(Ipv4Addr::LOCALHOST, BOARD), source)
                .unwrap();
            let (length, _) = udp.recv_from(&mut request).unwrap();
            let callback = std::str::from_utf8(&request[..length]).unwrap();
            let port = callback
                .strip_prefix("M66666 ")
                .unwrap()
                .parse::<u16>()
                .unwrap();

            let mut mqtt = TcpStream::connect((Ipv4Addr::LOCALHOST, port)).unwrap();
            mqtt_connect().write(&mut mqtt).unwrap();
            let _ = Packet::read(&mut mqtt).unwrap();
            mqtt_subscribe(BOARD).write(&mut mqtt).unwrap();
            let _ = Packet::read(&mut mqtt).unwrap();
            let command = PublishPacket::from_packet(&Packet::read(&mut mqtt).unwrap()).unwrap();
            let json: serde_json::Value = serde_json::from_slice(&command.data).unwrap();
            let url = json["Data"]["Data"]["Url"].as_str().unwrap();
            let after_host = url.strip_prefix("http://${ipaddr}:").unwrap();
            let (http_port, path) = after_host.split_once('/').unwrap();
            let mut http =
                TcpStream::connect((Ipv4Addr::LOCALHOST, http_port.parse::<u16>().unwrap()))
                    .unwrap();
            write!(http, "GET /{path} HTTP/1.1\r\nHost: mock\r\n\r\n").unwrap();
            let mut response = Vec::new();
            http.read_to_end(&mut response).unwrap();
            let body = response
                .windows(4)
                .position(|bytes| bytes == b"\r\n\r\n")
                .map(|position| &response[position + 4..])
                .unwrap();
            worker_pulled.lock().unwrap().extend_from_slice(body);

            let status = PublishPacket {
                flags: PublishFlags::QOS1,
                topic: format!("/sdcp/status/{BOARD}"),
                packet_id: Some(9),
                data: transfer_done(BOARD, "part.goo", expected.len() as u32),
            }
            .to_packet()
            .unwrap();
            status.write(&mut mqtt).unwrap();
            let _ = Packet::read(&mut mqtt).unwrap();
        });

        let progress = Arc::new(Mutex::new(Vec::new()));
        let callback_progress = progress.clone();
        let receipt = upload_artifact_inner(
            UploadRequest {
                target,
                resolution: Resolution { x: 3840, y: 2400 },
                filename: "part.goo".into(),
                artifact: artifact.clone(),
                probe_timeout: Duration::from_secs(1),
                connect_timeout: Duration::from_secs(1),
                transfer_timeout: Duration::from_secs(2),
            },
            || false,
            move |done, total| callback_progress.lock().unwrap().push((done, total)),
            true,
        )
        .unwrap();
        mock.join().unwrap();
        assert_eq!(*pulled.lock().unwrap(), artifact.as_ref());
        assert_eq!(receipt.bytes as usize, artifact.len());
        assert_eq!(
            *progress.lock().unwrap(),
            vec![(artifact.len() as u64, artifact.len() as u64)]
        );
    }
}
