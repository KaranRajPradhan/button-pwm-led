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
            let mut file = File::open("/dev/project_dev").expect("Failed to open device");
            let mut buf = String::new();
            file.read_to_string(&mut buf).expect("Failed to read");
            speed = buf.trim().parse::<i32>().unwrap_or(0);
            println!("Speed: {}", speed);
        }

        let (duty1, duty2, duty3) = match speed {
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
        };

        set_led(1, duty1);
        set_led(2, duty2);
        set_led(3, duty3);

        thread::sleep(Duration::from_millis(500)); // Poll every 500ms
    }
}

fn set_led(led: i32, duty: i32)
{
    let mut file = File::create("/dev/project_dev").expect("Failed to open device for writing");
    let cmd = format!("{} {}", led, duty);
    file.write_all(cmd.as_bytes()).expect("Failed to write");
}
