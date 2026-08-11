use std::{cmp::Ordering, fmt::Write as _};

use unicode_width::UnicodeWidthStr;

use crate::{DanmakuComment, DanmakuMode};

#[derive(Debug, Clone)]
pub struct AssConfig {
    pub play_res_x: u32,
    pub play_res_y: u32,
    pub font_name: String,
    pub font_size: f64,
    pub opacity: f64,
    pub scroll_duration_seconds: f64,
    pub fixed_duration_seconds: f64,
    pub display_area: f64,
    pub lane_gap: f64,
    pub time_offset_seconds: f64,
    pub blocked_terms: Vec<String>,
}

impl Default for AssConfig {
    fn default() -> Self {
        Self {
            play_res_x: 1920,
            play_res_y: 1080,
            font_name: "sans-serif".to_owned(),
            font_size: 42.0,
            opacity: 0.88,
            scroll_duration_seconds: 9.0,
            fixed_duration_seconds: 4.5,
            display_area: 0.70,
            lane_gap: 8.0,
            time_offset_seconds: 0.0,
            blocked_terms: Vec::new(),
        }
    }
}

pub struct AssGenerator;

impl AssGenerator {
    pub fn generate(comments: &[DanmakuComment], config: &AssConfig) -> String {
        let mut comments: Vec<_> = comments
            .iter()
            .filter(|comment| {
                !comment.text.trim().is_empty()
                    && !config
                        .blocked_terms
                        .iter()
                        .any(|term| !term.is_empty() && comment.text.contains(term))
            })
            .collect();
        comments.sort_by(|left, right| {
            left.time_seconds
                .partial_cmp(&right.time_seconds)
                .unwrap_or(Ordering::Equal)
        });

        let opacity = config.opacity.clamp(0.0, 1.0);
        let alpha = ((1.0 - opacity) * 255.0).round() as u8;
        let mut output = format!(
            "[Script Info]\nScriptType: v4.00+\nPlayResX: {}\nPlayResY: {}\nScaledBorderAndShadow: yes\nWrapStyle: 2\n\n[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\nStyle: Danmaku,{},{:.1},&H{:02X}FFFFFF,&H{:02X}FFFFFF,&H80000000,&H80000000,0,0,0,0,100,100,0,0,1,1.4,0,7,0,0,0,1\n\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n",
            config.play_res_x,
            config.play_res_y,
            escape_style_value(&config.font_name),
            config.font_size,
            alpha,
            alpha
        );

        let lane_height = (config.font_size + config.lane_gap).max(1.0);
        let usable_height = f64::from(config.play_res_y) * config.display_area.clamp(0.1, 1.0);
        let lane_count = (usable_height / lane_height).floor().max(1.0) as usize;
        let mut scroll_lanes = vec![0.0_f64; lane_count];
        let mut top_lanes = vec![0.0_f64; lane_count];
        let mut bottom_lanes = vec![0.0_f64; lane_count];
        let base_speed = f64::from(config.play_res_x) / config.scroll_duration_seconds.max(1.0);

        for comment in comments {
            let start = (comment.time_seconds + config.time_offset_seconds).max(0.0);
            let text = escape_text(&comment.text);
            let color = ass_bgr(comment.color_rgb);
            let visible_width =
                UnicodeWidthStr::width(comment.text.as_str()) as f64 * config.font_size * 0.55;

            let (lane, end, tag) = match comment.mode {
                DanmakuMode::Scroll => {
                    let lane = allocate_lane(&mut scroll_lanes, start);
                    let duration = (f64::from(config.play_res_x) + visible_width) / base_speed;
                    scroll_lanes[lane] = start + visible_width / base_speed + 0.12;
                    let y = lane_y(lane, lane_height);
                    (
                        lane,
                        start + duration,
                        format!(
                            "\\move({},{:.0},{:.0},{:.0},0,{})",
                            config.play_res_x,
                            y,
                            -visible_width,
                            y,
                            (duration * 1000.0).round() as u64
                        ),
                    )
                }
                DanmakuMode::Top => {
                    let lane = allocate_lane(&mut top_lanes, start);
                    let end = start + config.fixed_duration_seconds.max(0.5);
                    top_lanes[lane] = end;
                    let y = lane_y(lane, lane_height);
                    (
                        lane,
                        end,
                        format!("\\an8\\pos({},{:.0})", config.play_res_x / 2, y),
                    )
                }
                DanmakuMode::Bottom => {
                    let lane = allocate_lane(&mut bottom_lanes, start);
                    let end = start + config.fixed_duration_seconds.max(0.5);
                    bottom_lanes[lane] = end;
                    let y = f64::from(config.play_res_y) - lane_y(lane, lane_height);
                    (
                        lane,
                        end,
                        format!("\\an2\\pos({},{y:.0})", config.play_res_x / 2),
                    )
                }
            };

            let _ = lane;
            let _ = writeln!(
                output,
                "Dialogue: 1,{},{},Danmaku,,0,0,0,,{{\\fs{:.1}\\c&H{}&{}}}{}",
                ass_time(start),
                ass_time(end),
                config.font_size,
                color,
                tag,
                text
            );
        }
        output
    }
}

fn allocate_lane(lanes: &mut [f64], start: f64) -> usize {
    if let Some(index) = lanes.iter().position(|available_at| *available_at <= start) {
        index
    } else {
        lanes
            .iter()
            .enumerate()
            .min_by(|(_, left), (_, right)| left.partial_cmp(right).unwrap_or(Ordering::Equal))
            .map_or(0, |(index, _)| index)
    }
}

fn lane_y(lane: usize, lane_height: f64) -> f64 {
    lane_height * (lane as f64 + 0.7)
}

fn ass_time(seconds: f64) -> String {
    let centiseconds = (seconds.max(0.0) * 100.0).round() as u64;
    let hours = centiseconds / 360_000;
    let minutes = (centiseconds / 6_000) % 60;
    let secs = (centiseconds / 100) % 60;
    let fraction = centiseconds % 100;
    format!("{hours}:{minutes:02}:{secs:02}.{fraction:02}")
}

fn ass_bgr(rgb: u32) -> String {
    let red = (rgb >> 16) & 0xff;
    let green = (rgb >> 8) & 0xff;
    let blue = rgb & 0xff;
    format!("{blue:02X}{green:02X}{red:02X}")
}

fn escape_text(text: &str) -> String {
    text.replace('\\', "＼")
        .replace('{', "｛")
        .replace('}', "｝")
        .replace(['\r', '\n'], " ")
}

fn escape_style_value(value: &str) -> String {
    value.replace(',', " ").replace(['\r', '\n'], " ")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn produces_secondary_ass_track_and_escapes_tags() {
        let comments = vec![
            DanmakuComment {
                time_seconds: 1.25,
                mode: DanmakuMode::Scroll,
                color_rgb: 0x00ff_0000,
                text: "hello {\\b1}".into(),
                source_id: None,
                sender: None,
            },
            DanmakuComment {
                time_seconds: 2.0,
                mode: DanmakuMode::Top,
                color_rgb: 0x0000_ff00,
                text: "top".into(),
                source_id: None,
                sender: None,
            },
        ];

        let output = AssGenerator::generate(&comments, &AssConfig::default());
        assert!(output.starts_with("[Script Info]"));
        assert!(output.contains("Dialogue: 1,0:00:01.25"));
        assert!(output.contains("&H0000FF&"));
        assert!(output.contains("hello ｛＼b1｝"));
        assert!(output.contains("\\an8\\pos"));
    }

    #[test]
    fn filters_blocked_terms() {
        let comments = vec![DanmakuComment {
            time_seconds: 0.0,
            mode: DanmakuMode::Bottom,
            color_rgb: 0x00ff_ffff,
            text: "spoiler".into(),
            source_id: None,
            sender: None,
        }];
        let config = AssConfig {
            blocked_terms: vec!["spoiler".into()],
            ..AssConfig::default()
        };

        assert!(!AssGenerator::generate(&comments, &config).contains("spoiler"));
    }
}
