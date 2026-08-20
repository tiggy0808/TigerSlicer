//! Bounded, explicit SDCP printer probing.
//!
//! This module deliberately has no background service or discovery side effect.
//! A socket exists only for the duration of [`probe_printer`], and replies are
//! accepted only from the exact configured endpoint.

use std::{
    fmt,
    net::{Ipv4Addr, SocketAddr, SocketAddrV4, UdpSocket},
    sync::atomic::{AtomicBool, Ordering},
    time::{Duration, Instant},
};

use crate::{
    Resolution, Response,
    status::{Capability, FullStatusData},
};

const PROBE_MESSAGE: &[u8] = b"M99999";
const MAX_STATUS_DATAGRAM: usize = 8 * 1024;
const POLL_INTERVAL: Duration = Duration::from_millis(50);
const MAX_ID_LEN: usize = 128;
const MAX_NAME_LEN: usize = 256;
const MAX_VERSION_LEN: usize = 64;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PrinterIdentity {
    pub name: String,
    pub machine_name: String,
    pub mainboard_id: String,
    pub firmware_version: String,
    pub protocol_version: String,
    pub resolution: Resolution,
    pub file_transfer: bool,
    pub print_control: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ProbeError {
    InvalidAddress,
    Timeout,
    Cancelled,
    Io(String),
    WrongSource,
    OversizedResponse,
    MalformedResponse,
    IdentityMismatch,
    InvalidIdentity,
    ResolutionMismatch,
    FileTransferUnsupported,
}

impl fmt::Display for ProbeError {
    fn fmt(&self, out: &mut fmt::Formatter<'_>) -> fmt::Result {
        let message = match self {
            Self::InvalidAddress => "printer address must be private or link-local IPv4 unicast",
            Self::Timeout => "printer did not answer before the probe deadline",
            Self::Cancelled => "printer probe was cancelled",
            Self::Io(message) => return write!(out, "printer probe I/O error: {message}"),
            Self::WrongSource => "printer reply came from an unexpected address",
            Self::OversizedResponse => "printer status response exceeds the safety limit",
            Self::MalformedResponse => "printer returned malformed SDCP status",
            Self::IdentityMismatch => {
                "printer status identity or address does not match the configured printer"
            }
            Self::InvalidIdentity => {
                "printer status contains an invalid or oversized identity field"
            }
            Self::ResolutionMismatch => {
                "printer resolution does not match the active resin profile"
            }
            Self::FileTransferUnsupported => "printer does not advertise SDCP file transfer",
        };
        out.write_str(message)
    }
}

impl std::error::Error for ProbeError {}

pub fn is_allowed_printer_address(address: Ipv4Addr) -> bool {
    (address.is_private() || address.is_link_local())
        && !address.is_unspecified()
        && !address.is_loopback()
        && !address.is_multicast()
        && address != Ipv4Addr::BROADCAST
}

pub fn probe_printer(
    target: SocketAddrV4,
    expected_resolution: Resolution,
    timeout: Duration,
    cancelled: &AtomicBool,
) -> Result<PrinterIdentity, ProbeError> {
    probe_printer_with_cancel(target, expected_resolution, timeout, || {
        cancelled.load(Ordering::Acquire)
    })
}

pub fn probe_printer_with_cancel(
    target: SocketAddrV4,
    expected_resolution: Resolution,
    timeout: Duration,
    cancelled: impl Fn() -> bool,
) -> Result<PrinterIdentity, ProbeError> {
    probe_printer_inner(target, expected_resolution, timeout, cancelled, false)
}

pub(crate) fn probe_printer_inner(
    target: SocketAddrV4,
    expected_resolution: Resolution,
    timeout: Duration,
    cancelled: impl Fn() -> bool,
    allow_loopback: bool,
) -> Result<PrinterIdentity, ProbeError> {
    if timeout.is_zero()
        || !(is_allowed_printer_address(*target.ip())
            || allow_loopback && target.ip().is_loopback())
    {
        return Err(ProbeError::InvalidAddress);
    }
    if cancelled() {
        return Err(ProbeError::Cancelled);
    }

    // Ask the OS which interface routes to the configured printer, then bind
    // the receiving socket to that interface rather than all interfaces.
    let route = UdpSocket::bind((Ipv4Addr::UNSPECIFIED, 0)).map_err(io_error)?;
    route.connect(target).map_err(io_error)?;
    let local_ip = match route.local_addr().map_err(io_error)? {
        SocketAddr::V4(address) => *address.ip(),
        SocketAddr::V6(_) => return Err(ProbeError::InvalidAddress),
    };
    drop(route);

    let socket = UdpSocket::bind((local_ip, 0)).map_err(io_error)?;
    socket.connect(target).map_err(io_error)?;
    socket.send(PROBE_MESSAGE).map_err(io_error)?;

    let started = Instant::now();
    let mut buffer = [0_u8; MAX_STATUS_DATAGRAM + 1];
    loop {
        if cancelled() {
            return Err(ProbeError::Cancelled);
        }
        let remaining = timeout
            .checked_sub(started.elapsed())
            .ok_or(ProbeError::Timeout)?;
        socket
            .set_read_timeout(Some(remaining.min(POLL_INTERVAL)))
            .map_err(io_error)?;

        match socket.recv_from(&mut buffer) {
            Ok((_, source)) if source != SocketAddr::V4(target) => {
                return Err(ProbeError::WrongSource);
            }
            Ok((length, _)) if length > MAX_STATUS_DATAGRAM => {
                return Err(ProbeError::OversizedResponse);
            }
            Ok((length, _)) => {
                let response: Response<FullStatusData> = serde_json::from_slice(&buffer[..length])
                    .map_err(|_| ProbeError::MalformedResponse)?;
                return validate_response(response, target, expected_resolution);
            }
            Err(error)
                if matches!(
                    error.kind(),
                    std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
                ) => {}
            Err(error) => return Err(io_error(error)),
        }
    }
}

fn validate_response(
    response: Response<FullStatusData>,
    target: SocketAddrV4,
    expected_resolution: Resolution,
) -> Result<PrinterIdentity, ProbeError> {
    let attributes = response.data.attributes;
    let reported_ip = attributes
        .mainboard_ip
        .parse::<Ipv4Addr>()
        .map_err(|_| ProbeError::IdentityMismatch)?;
    if reported_ip != *target.ip() || response.id != attributes.mainboard_id {
        return Err(ProbeError::IdentityMismatch);
    }
    if !bounded_text(&attributes.mainboard_id, MAX_ID_LEN)
        || !bounded_text(&attributes.name, MAX_NAME_LEN)
        || !bounded_text(&attributes.machine_name, MAX_NAME_LEN)
        || !bounded_text(&attributes.firmware_version, MAX_VERSION_LEN)
        || !bounded_text(&attributes.protocol_version, MAX_VERSION_LEN)
    {
        return Err(ProbeError::InvalidIdentity);
    }
    if attributes.resolution != expected_resolution {
        return Err(ProbeError::ResolutionMismatch);
    }
    let file_transfer = attributes
        .capabilities
        .iter()
        .any(|item| matches!(item, Capability::FileTransfer));
    if !file_transfer {
        return Err(ProbeError::FileTransferUnsupported);
    }

    Ok(PrinterIdentity {
        name: attributes.name,
        machine_name: attributes.machine_name,
        mainboard_id: attributes.mainboard_id,
        firmware_version: attributes.firmware_version,
        protocol_version: attributes.protocol_version,
        resolution: attributes.resolution,
        file_transfer,
        print_control: attributes
            .capabilities
            .iter()
            .any(|item| matches!(item, Capability::PrintControl)),
    })
}

fn bounded_text(value: &str, max_len: usize) -> bool {
    !value.is_empty() && value.len() <= max_len && !value.chars().any(char::is_control)
}

fn io_error(error: std::io::Error) -> ProbeError {
    ProbeError::Io(error.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{net::UdpSocket, sync::Arc, thread};

    fn status(ip: Ipv4Addr, resolution: &str, id: &str, capabilities: &str) -> Vec<u8> {
        format!(r#"{{"Id":"{id}","Data":{{"Attributes":{{"Name":"Mock","MachineName":"Mock Resin","ProtocolVersion":"V3.0.0","FirmwareVersion":"1.0","Resolution":"{resolution}","MainboardIP":"{ip}","MainboardID":"{id}","SDCPStatus":0,"LocalSDCPAddress":"","SDCPAddress":"","Capabilities":[{capabilities}]}},"Status":{{"CurrentStatus":0,"PreviousStatus":0,"PrintInfo":{{"Status":0,"CurrentLayer":0,"TotalLayer":0,"CurrentTicks":0,"TotalTicks":0,"ErrorNumber":0,"Filename":""}},"FileTransferInfo":{{"Status":0,"DownloadOffset":0,"CheckOffset":0,"FileTotalSize":0,"Filename":""}}}}}}}}"#).into_bytes()
    }

    fn mock_probe(reply: Vec<u8>) -> Result<PrinterIdentity, ProbeError> {
        let server = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).unwrap();
        let target = match server.local_addr().unwrap() {
            SocketAddr::V4(v) => v,
            _ => unreachable!(),
        };
        let worker = thread::spawn(move || {
            let mut request = [0_u8; 16];
            let (length, source) = server.recv_from(&mut request).unwrap();
            assert_eq!(&request[..length], PROBE_MESSAGE);
            server.send_to(&reply, source).unwrap();
        });
        let result = probe_printer_inner(
            target,
            Resolution { x: 3840, y: 2400 },
            Duration::from_secs(1),
            || false,
            true,
        );
        worker.join().unwrap();
        result
    }

    #[test]
    fn accepts_matching_file_transfer_printer() {
        let identity = mock_probe(status(
            Ipv4Addr::LOCALHOST,
            "3840x2400",
            "board-1",
            "\"FILE_TRANSFER\",\"PRINT_CONTROL\"",
        ))
        .unwrap();
        assert_eq!(identity.mainboard_id, "board-1");
        assert!(identity.file_transfer);
        assert!(identity.print_control);
    }

    #[test]
    fn rejects_resolution_and_capability_mismatches() {
        assert_eq!(
            mock_probe(status(
                Ipv4Addr::LOCALHOST,
                "1920x1080",
                "board-1",
                "\"FILE_TRANSFER\""
            )),
            Err(ProbeError::ResolutionMismatch)
        );
        assert_eq!(
            mock_probe(status(
                Ipv4Addr::LOCALHOST,
                "3840x2400",
                "board-1",
                "\"PRINT_CONTROL\""
            )),
            Err(ProbeError::FileTransferUnsupported)
        );
    }

    #[test]
    fn rejects_identity_mismatch_and_oversized_fields() {
        let mismatched = String::from_utf8(status(
            Ipv4Addr::LOCALHOST,
            "3840x2400",
            "board-1",
            "\"FILE_TRANSFER\"",
        ))
        .unwrap()
        .replacen("\"Id\":\"board-1\"", "\"Id\":\"other-board\"", 1)
        .into_bytes();
        assert_eq!(mock_probe(mismatched), Err(ProbeError::IdentityMismatch));
        let long_id = "x".repeat(MAX_ID_LEN + 1);
        assert_eq!(
            mock_probe(status(
                Ipv4Addr::LOCALHOST,
                "3840x2400",
                &long_id,
                "\"FILE_TRANSFER\""
            )),
            Err(ProbeError::InvalidIdentity)
        );
    }

    #[test]
    fn validates_production_address_policy() {
        assert!(is_allowed_printer_address(Ipv4Addr::new(192, 168, 1, 2)));
        assert!(is_allowed_printer_address(Ipv4Addr::new(10, 0, 0, 2)));
        assert!(is_allowed_printer_address(Ipv4Addr::new(169, 254, 1, 2)));
        assert!(!is_allowed_printer_address(Ipv4Addr::LOCALHOST));
        assert!(!is_allowed_printer_address(Ipv4Addr::new(8, 8, 8, 8)));
        assert!(!is_allowed_printer_address(Ipv4Addr::BROADCAST));
    }

    #[test]
    fn cancellation_prevents_network_activity() {
        let cancelled = Arc::new(AtomicBool::new(true));
        let result = probe_printer_inner(
            SocketAddrV4::new(Ipv4Addr::LOCALHOST, 9),
            Resolution { x: 1, y: 1 },
            Duration::from_secs(1),
            || cancelled.load(Ordering::Acquire),
            true,
        );
        assert_eq!(result, Err(ProbeError::Cancelled));
    }

    #[test]
    fn rejects_malformed_and_oversized_responses() {
        assert_eq!(
            mock_probe(b"not-json".to_vec()),
            Err(ProbeError::MalformedResponse)
        );
        assert_eq!(
            mock_probe(vec![b'x'; MAX_STATUS_DATAGRAM + 1]),
            Err(ProbeError::OversizedResponse)
        );
    }

    #[test]
    fn obeys_probe_deadline() {
        let server = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).unwrap();
        let target = match server.local_addr().unwrap() {
            SocketAddr::V4(v) => v,
            _ => unreachable!(),
        };
        let worker = thread::spawn(move || {
            let mut request = [0_u8; 16];
            let _ = server.recv_from(&mut request).unwrap();
            thread::sleep(Duration::from_millis(100));
        });
        let result = probe_printer_inner(
            target,
            Resolution { x: 3840, y: 2400 },
            Duration::from_millis(20),
            || false,
            true,
        );
        worker.join().unwrap();
        assert_eq!(result, Err(ProbeError::Timeout));
    }
}
