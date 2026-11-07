//! The Meowy Launcher's logic, apart from its windows (`launcher/app`).
//! See `launcher/DESIGN.md`.

pub mod a2s;
pub mod address;
#[cfg(windows)]
pub mod http;
pub mod index;
pub mod servers;
pub mod line;
pub mod links;
pub mod pipe;
pub mod log;
pub mod policy;
pub mod session;
pub mod settings;
pub mod steam;
pub mod updater;
#[cfg(windows)]
pub mod win;
