//! The Meowy Launcher's logic, apart from its windows (`launcher/app`).
//! See `launcher/DESIGN.md`.

pub mod address;
pub mod line;
pub mod links;
pub mod log;
pub mod session;
pub mod settings;
pub mod steam;
pub mod updater;
#[cfg(windows)]
pub mod win;
