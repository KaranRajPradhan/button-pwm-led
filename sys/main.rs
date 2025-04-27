use std::fs::{File, OpenOptions};
use std::io::{Read, Write};
use std::thread;
use std::time::Duration;

fn read_speed() -> u32 {
    let mut file = match File::open("/sys/kernel/project_sys/speed") {
        Ok(f) => f,
        Err(_) => return 0,
    };

    let mut contents = String::new();
    if file.read_to_string(&mut contents).is_ok() {
        contents.trim().parse::<u32>().unwrap_or(0)
    } else {
        0
    }
}

fn write_led(led: &str, duty: u32) {
    if let Ok(mut file) = OpenOptions::new().write(true).open(format!("/sys/kernel/project_sys/{}", led)) {
        let _ = writeln!(file, "{}", duty);
    }
}

fn map_speed_to_leds(speed: u32) -> (u32, u32, u32) {
    match speed {
        0 => (0, 0, 0),
        1..=5 => (25, 0, 0),
        6..=10 => (50, 0, 0),
        11..=15 => (75, 0, 0),
        16..=20 => (100, 0, 0),
        21..=25 => (100, 25, 0),
        26..=30 => (100, 50, 0),
        31..=35 => (100, 75, 0),
        36..=40 => (100, 100, 0),
        41..=45 => (100, 100, 25),
        46..=50 => (100, 100, 50),
        51..=55 => (100, 100, 75),
        56..=60 => (100, 100, 100),
        _ => (100, 100, 100),
    }
}

fn main() {
    loop {
        let speed = read_speed();
        let (led1_duty, led2_duty, led3_duty) = map_speed_to_leds(speed);

        write_led("led1", led1_duty);
        write_led("led2", led2_duty);
        write_led("led3", led3_duty);

        thread::sleep(Duration::from_secs(1));
    }
}
