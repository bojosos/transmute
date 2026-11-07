use std::thread;

fn main() {
    let handles: Vec<_> = (0..4).map(|i| thread::spawn(move || i * 10)).collect();
    let sum: i32 = handles.into_iter().map(|h| h.join().unwrap()).sum();
    let args: Vec<String> = std::env::args().collect();
    println!("hello from rustc, threads sum={}, args={}", sum, args.len());
}
