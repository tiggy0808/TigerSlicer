use anyhow::Result;
use bitflags::bitflags;

use common::serde::{DynamicSerializer, Serializer};

use crate::mqtt::misc::MqttSerializer;

use super::{Packet, PacketCursor};

#[derive(Debug)]
pub struct PublishPacket {
    pub flags: PublishFlags,
    pub topic: String,
    pub packet_id: Option<u16>,
    pub data: Vec<u8>,
}

bitflags! {
    #[derive(Debug)]
    pub struct PublishFlags: u8 {
        const DUP = 0b1000;
        const QOS1 = 0b0010;
        const QOS2 = 0b0100;
        const RETAIN = 0b0001;
    }
}

impl PublishPacket {
    pub const PACKET_TYPE: u8 = 0x03;

    pub fn from_packet(packet: &Packet) -> Result<Self> {
        anyhow::ensure!(
            packet.packet_type == Self::PACKET_TYPE,
            "invalid MQTT PUBLISH type"
        );
        let mut des = PacketCursor::new(&packet.remaining_bytes);

        let flags = PublishFlags::from_bits(packet.flags)
            .ok_or_else(|| anyhow::anyhow!("invalid MQTT publish flags"))?;
        anyhow::ensure!(
            !(flags.contains(PublishFlags::QOS1) && flags.contains(PublishFlags::QOS2)),
            "invalid MQTT publish QoS"
        );
        let topic = des.string()?;

        let packet_id = (flags.contains(PublishFlags::QOS1) || flags.contains(PublishFlags::QOS2))
            .then(|| des.u16())
            .transpose()?;
        let data = des.rest().to_vec();

        Ok(Self {
            flags,
            topic,
            packet_id,
            data,
        })
    }

    pub fn to_packet(&self) -> Result<Packet> {
        let mut ser = DynamicSerializer::new();
        ser.write_string(&self.topic);
        if self.flags.contains(PublishFlags::QOS1) || self.flags.contains(PublishFlags::QOS2) {
            let packet_id = self
                .packet_id
                .ok_or_else(|| anyhow::anyhow!("QoS MQTT publish has no packet ID"))?;
            ser.write_u16_be(packet_id);
        }
        ser.write_bytes(&self.data);

        let data = ser.into_inner();
        anyhow::ensure!(
            data.len() <= super::MAX_PACKET_BYTES,
            "MQTT publish exceeds safety limit"
        );
        Ok(Packet {
            packet_type: Self::PACKET_TYPE,
            flags: self.flags.bits(),
            remaining_length: data.len() as u32,
            remaining_bytes: data,
        })
    }
}
