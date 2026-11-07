"use strict";
// What the card window says for each kind of card, as plain data, so
// tests/launcher_card_test.js can check it with Node. The page only ever sets
// textContent from this, never HTML.

function cardView(card) {
  if (!card) return { title: "", text: "", extra: "", buttons: [{ label: "Close", action: "dismiss", style: "" }] };
  switch (card.kind) {
    case "crash": {
      const where = card.name ? card.name + " (" + card.address + ")" : card.address;
      return {
        title: "Garry's Mod closed unexpectedly",
        text: "You were on " + where + ". Jump back in?",
        extra: card.report ? "A crash report was written." : "No crash report was written (the game was ended or froze).",
        buttons: [
          { label: "Not now", action: "dismiss", style: "ghost" },
          ...(card.report ? [{ label: "Details", action: "details", style: "" }] : []),
          { label: "Rejoin", action: "join", style: "primary" },
        ],
      };
    }
    case "confirm":
      return {
        title: "Join this server?",
        text: "A link asks to join " + card.address + ", which isn't a Meowy Network server.",
        extra: "Only join servers you trust.",
        buttons: [
          { label: "Cancel", action: "dismiss", style: "ghost" },
          { label: "Join", action: "join", style: "primary" },
        ],
      };
    case "back_up":
      return {
        title: "Meowy Network is back up",
        text: (card.name || "The server") + " answers again.",
        extra: "",
        buttons: [
          { label: "Not now", action: "dismiss", style: "ghost" },
          { label: "Join", action: "join", style: "primary" },
        ],
      };
    case "notice":
      return { title: card.title, text: card.text, extra: "", buttons: [{ label: "OK", action: "dismiss", style: "primary" }] };
    default:
      return { title: "", text: "", extra: "", buttons: [{ label: "Close", action: "dismiss", style: "" }] };
  }
}

if (typeof module !== "undefined") module.exports = { cardView };
