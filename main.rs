use std::fs::File;
use std::io::{Read, Write};
use std::thread;
use std::time::Duration;

fn main()
{
    loop
    {
        let mut speed = 0;

        // Read speed
        {
            let mut file = File::open("/dev/project_dev").expect("");
            let mut buf = String::new();
            file.read_to_string(&mut buf).expect("");
            speed = buf.trim().parse::<i32>().unwrap_or(0);
            println!("Speed: {}", speed);
        }

        let level = (speed * 15) / 100;

        let (led, duty) = match level {
            0..=4 => (1, level_to_duty(level)),
            5..=9 => (2, level_to_duty(level - 5)),
            10..=14 => (3, level_to_duty(level - 10)),
            _ => (1, 0), // Default to LED1 off if out of bounds
        };

        set_leds(led, duty);

        thread::sleep(Duration::from_millis(500)); // Poll every 500ms
    }
}

fn level_to_duty(level: i32) -> i32
{
    match level {
        0 => 0,
        1 => 25,
        2 => 50,
        3 => 75,
        4 => 100,
        _ => 0,
    }
}

fn set_leds(active_led: i32, duty: i32)
{
    for led in 1..=3
    {
        let mut file = File::create("/dev/project_dev").expect("");

        if led == active_led {
            let cmd = format!("{} {}", led, duty);
            file.write_all(cmd.as_bytes()).expect("");
        } else {
            let cmd = format!("{} 0", led);
            file.write_all(cmd.as_bytes()).expect("");
        }
    }
}
