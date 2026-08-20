use anyhow::Result;

use common::serde::{DynamicSerializer, Serializer};

use super::{Packet, PacketCursor};

pub struct PublishAckPacket {
    pub packet_id: u16,
}

impl PublishAckPacket {
    pub const PACKET_TYPE: u8 = 0x04;

    pub fn to_packet(&self) -> Packet {
        let mut ser = DynamicSerializer::new();
        ser.write_u16_be(self.packet_id);

        let data = ser.into_inner();
        Packet {
            packet_type: Self::PACKET_TYPE,
            flags: 0,
            remaining_length: data.len() as u32,
            remaining_bytes: data,
        }
    }

    pub fn from_packet(packet: &Packet) -> Result<Self> {
        anyhow::ensure!(
            packet.packet_type == Self::PACKET_TYPE && packet.flags == 0,
            "invalid MQTT PUBACK header"
        );
        let mut des = PacketCursor::new(&packet.remaining_bytes);
        let packet_id = des.u16()?;
        anyhow::ensure!(des.is_empty(), "trailing MQTT PUBACK data");

        Ok(Self { packet_id })
    }
}
