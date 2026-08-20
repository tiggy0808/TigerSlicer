use anyhow::Result;

use super::{Packet, PacketCursor, QoS};

#[derive(Debug)]
pub struct SubscribePacket {
    pub packet_id: u16,
    pub filters: Vec<(String, QoS)>,
}

impl SubscribePacket {
    pub const PACKET_TYPE: u8 = 0x08;

    pub fn from_packet(packet: &Packet) -> Result<Self> {
        anyhow::ensure!(
            packet.packet_type == Self::PACKET_TYPE && packet.flags == 2,
            "invalid MQTT SUBSCRIBE header"
        );
        let mut des = PacketCursor::new(&packet.remaining_bytes);

        let packet_id = des.u16()?;
        let mut filters = Vec::new();
        while !des.is_empty() {
            anyhow::ensure!(filters.len() < 16, "too many MQTT topic filters");
            let topic = des.string()?;
            let qos = des.u8()?;
            anyhow::ensure!(qos <= 1, "unsupported MQTT subscription QoS");
            filters.push((topic, QoS(qos)));
        }

        anyhow::ensure!(!filters.is_empty(), "MQTT subscription has no filters");

        Ok(Self { packet_id, filters })
    }
}
