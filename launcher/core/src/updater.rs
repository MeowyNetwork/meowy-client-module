//! UPDATER-STUB.
//!
//! The launcher has no update channel of its own. It will use the pack's
//! signed release and policy files (U1/K1, being built inside the pack by
//! the main session): fetch them outside the game, verify them with the same
//! public key (BCrypt ECDSA P-256 / SHA-256, as `src/notify_catalogue.cpp`),
//! stage a verified version where the pack's own updater would, and obey the
//! kill switch's off-switches for the launcher's features too. See
//! `launcher/DESIGN.md` §8.
//!
//! Until that format is merged, this does nothing and touches no network.
//! Before replacing it, read the pack's updater PR and reuse its format,
//! keys and staging folder as they are.

/// What the launcher shows under "Updates".
pub fn status() -> &'static str {
    "Updates come with the Meowy Client Pack installer for now."
}

/// The interface the real updater fills in: check, stage, report.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Check {
    /// No update channel yet (this stub).
    Unavailable,
}

pub fn check() -> Check {
    Check::Unavailable
}
