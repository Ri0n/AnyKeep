use private_notes_xmpp_conformance::verify_encoded_document;
use serde_json::Value;
use std::{env, fs, process};

fn main() {
    let path = env::args().nth(1).unwrap_or_else(|| {
        eprintln!("usage: cargo run -- <python-encoded.json>");
        process::exit(2);
    });
    let text = fs::read_to_string(&path).unwrap_or_else(|error| {
        eprintln!("cannot read {path}: {error}");
        process::exit(2);
    });
    let document: Value = serde_json::from_str(&text).unwrap_or_else(|error| {
        eprintln!("invalid JSON: {error}");
        process::exit(2);
    });
    match verify_encoded_document(&document) {
        Ok(result) => println!("{}", serde_json::to_string_pretty(&result).expect("JSON output")),
        Err(error) => {
            eprintln!("validation failed: {error}");
            process::exit(1);
        }
    }
}
