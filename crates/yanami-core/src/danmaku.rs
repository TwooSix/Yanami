use serde::{Deserialize, Serialize};

/// The layout mode requested by a timed comment.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum DanmakuMode {
    Scroll,
    Bottom,
    Top,
}

/// A presentation-independent timed comment.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DanmakuComment {
    pub time_seconds: f64,
    pub mode: DanmakuMode,
    pub color_rgb: u32,
    pub text: String,
    pub source_id: Option<i64>,
    pub sender: Option<String>,
}
