use common::{id_type, serde::Serializer};

pub trait MqttSerializer {
    fn write_string(&mut self, data: &str);
}

impl<T> MqttSerializer for T
where
    T: Serializer,
{
    fn write_string(&mut self, data: &str) {
        assert!(data.len() <= u16::MAX as usize);
        let len = data.len() as u16;
        self.write_u16_be(len);
        self.write_bytes(data.as_bytes());
    }
}

id_type!(ClientId, u64);
