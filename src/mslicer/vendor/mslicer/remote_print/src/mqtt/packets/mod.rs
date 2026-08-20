use std::io::{Read, Write};

use anyhow::{Result, bail, ensure};

pub mod connect;
pub mod connect_ack;
pub mod publish;
pub mod publish_ack;
pub mod subscribe;
pub mod subscribe_ack;

pub struct Packet {
    pub packet_type: u8,
    pub flags: u8,
    pub remaining_length: u32,
    pub remaining_bytes: Vec<u8>,
}

#[derive(Debug, Clone, Copy)]
pub struct QoS(pub u8);

pub const MAX_PACKET_BYTES: usize = 64 * 1024;

pub(super) struct PacketCursor<'a> {
    bytes: &'a [u8],
    position: usize,
}

impl<'a> PacketCursor<'a> {
    pub fn new(bytes: &'a [u8]) -> Self {
        Self { bytes, position: 0 }
    }
    pub fn is_empty(&self) -> bool {
        self.position == self.bytes.len()
    }
    pub fn u8(&mut self) -> Result<u8> {
        let value = *self
            .bytes
            .get(self.position)
            .ok_or_else(|| anyhow::anyhow!("truncated MQTT packet"))?;
        self.position += 1;
        Ok(value)
    }
    pub fn u16(&mut self) -> Result<u16> {
        let bytes = self.take(2)?;
        Ok(u16::from_be_bytes([bytes[0], bytes[1]]))
    }
    pub fn string(&mut self) -> Result<String> {
        let length = self.u16()? as usize;
        ensure!(length <= 4096, "MQTT string exceeds safety limit");
        let bytes = self.take(length)?;
        Ok(std::str::from_utf8(bytes)
            .map_err(|_| anyhow::anyhow!("MQTT string is not UTF-8"))?
            .to_owned())
    }
    pub fn rest(&mut self) -> &'a [u8] {
        let output = &self.bytes[self.position..];
        self.position = self.bytes.len();
        output
    }
    fn take(&mut self, length: usize) -> Result<&'a [u8]> {
        let end = self
            .position
            .checked_add(length)
            .ok_or_else(|| anyhow::anyhow!("MQTT field length overflow"))?;
        let output = self
            .bytes
            .get(self.position..end)
            .ok_or_else(|| anyhow::anyhow!("truncated MQTT packet"))?;
        self.position = end;
        Ok(output)
    }
}

impl Packet {
    pub fn write<Stream: Write>(&self, stream: &mut Stream) -> Result<()> {
        ensure!(
            self.remaining_bytes.len() <= MAX_PACKET_BYTES,
            "MQTT packet exceeds safety limit"
        );
        ensure!(
            self.remaining_length as usize == self.remaining_bytes.len(),
            "MQTT packet length mismatch"
        );
        let mut bytes = vec![self.packet_type << 4 | self.flags];
        let mut remaining_length = self.remaining_length;
        loop {
            let mut byte = (remaining_length % 128) as u8;
            remaining_length /= 128;
            if remaining_length > 0 {
                byte |= 0x80;
            }
            bytes.push(byte);
            if remaining_length == 0 {
                break;
            }
        }
        bytes.extend(self.remaining_bytes.iter());

        stream.write_all(&bytes)?;
        Ok(())
    }

    pub fn read<Stream: Read>(stream: &mut Stream) -> Result<Self> {
        let mut first = [0_u8; 1];
        stream.read_exact(&mut first)?;
        let (packet_type, flags) = (first[0] >> 4, first[0] & 0xF);
        ensure!(packet_type != 0, "invalid MQTT packet type");
        let mut remaining_length = 0_usize;
        let mut multiplier = 1_usize;
        for position in 0..4 {
            let mut encoded = [0_u8; 1];
            stream.read_exact(&mut encoded)?;
            remaining_length = remaining_length
                .checked_add(((encoded[0] & 0x7f) as usize) * multiplier)
                .ok_or_else(|| anyhow::anyhow!("MQTT packet length overflow"))?;
            ensure!(
                remaining_length <= MAX_PACKET_BYTES,
                "MQTT packet exceeds safety limit"
            );
            let byte = encoded[0];
            if byte & 0x80 == 0 {
                break;
            }
            if position == 3 {
                bail!("invalid MQTT remaining-length encoding");
            }
            multiplier *= 128;
        }

        let mut remaining_bytes = vec![0; remaining_length];
        stream.read_exact(&mut remaining_bytes)?;

        Ok(Self {
            packet_type,
            flags,
            remaining_length: remaining_length as u32,
            remaining_bytes,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mqtt::packets::{connect::ConnectPacket, publish::PublishPacket};
    use std::io::Cursor;

    #[test]
    fn rejects_oversized_and_invalid_remaining_lengths_before_allocation() {
        assert!(Packet::read(&mut Cursor::new([0x30, 0x81, 0x80, 0x04])).is_err());
        assert!(Packet::read(&mut Cursor::new([0x30, 0xff, 0xff, 0xff, 0xff])).is_err());
    }

    #[test]
    fn rejects_truncated_and_invalid_typed_packets_without_panicking() {
        let truncated = Packet {
            packet_type: ConnectPacket::PACKET_TYPE,
            flags: 0,
            remaining_length: 1,
            remaining_bytes: vec![0],
        };
        assert!(ConnectPacket::from_packet(&truncated).is_err());
        let invalid_publish = Packet {
            packet_type: PublishPacket::PACKET_TYPE,
            flags: 0x06,
            remaining_length: 2,
            remaining_bytes: vec![0, 0],
        };
        assert!(PublishPacket::from_packet(&invalid_publish).is_err());
    }

    #[test]
    fn packet_writer_rejects_length_mismatch() {
        let packet = Packet {
            packet_type: 3,
            flags: 0,
            remaining_length: 1,
            remaining_bytes: Vec::new(),
        };
        assert!(packet.write(&mut Vec::new()).is_err());
    }
}
