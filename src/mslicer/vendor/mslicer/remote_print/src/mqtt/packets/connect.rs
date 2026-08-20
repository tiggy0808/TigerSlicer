use anyhow::Result;
use bitflags::bitflags;

use super::{Packet, PacketCursor};

#[derive(Debug)]
pub struct ConnectPacket {
    pub protocol_name: String,
    pub protocol_level: u8,
    pub connect_flags: ConnectFlags,
    pub keep_alive: u16,

    pub client_id: String,
    pub will_topic: Option<String>,
    pub will_message: Option<String>,
    pub username: Option<String>,
    pub password: Option<String>,
}

bitflags! {
    #[derive(Debug)]
    pub struct ConnectFlags: u8 {
        const USERNAME = 0b10000000;
        const PASSWORD = 0b01000000;
        const WILL_RETAIN = 0b00100000;
        const WILL_QOS = 0b00011000;
        const WILL_FLAG = 0b00000100;
        const CLEAN_SESSION = 0b00000010;
        const RESERVED = 0b00000001;
    }
}

impl ConnectPacket {
    pub const PACKET_TYPE: u8 = 0x01;

    pub fn from_packet(packet: &Packet) -> Result<Self> {
        anyhow::ensure!(
            packet.packet_type == Self::PACKET_TYPE && packet.flags == 0,
            "invalid MQTT CONNECT header"
        );
        let mut des = PacketCursor::new(&packet.remaining_bytes);

        let protocol_name = des.string()?;
        let protocol_level = des.u8()?;
        let connect_flags = ConnectFlags::from_bits(des.u8()?)
            .ok_or_else(|| anyhow::anyhow!("invalid MQTT connect flags"))?;
        anyhow::ensure!(
            !connect_flags.contains(ConnectFlags::RESERVED),
            "reserved MQTT connect flag is set"
        );
        let keep_alive = des.u16()?;

        let client_id = des.string()?;
        let will_topic = connect_flags
            .contains(ConnectFlags::WILL_FLAG)
            .then(|| des.string())
            .transpose()?;
        let will_message = connect_flags
            .contains(ConnectFlags::WILL_FLAG)
            .then(|| des.string())
            .transpose()?;
        let username = connect_flags
            .contains(ConnectFlags::USERNAME)
            .then(|| des.string())
            .transpose()?;
        let password = connect_flags
            .contains(ConnectFlags::PASSWORD)
            .then(|| des.string())
            .transpose()?;
        anyhow::ensure!(des.is_empty(), "trailing MQTT CONNECT data");

        Ok(Self {
            protocol_name,
            protocol_level,
            connect_flags,
            keep_alive,
            client_id,
            will_topic,
            will_message,
            username,
            password,
        })
    }
}
