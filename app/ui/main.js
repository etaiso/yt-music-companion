// Placeholder frontend for the YT Music board bridge tray app.
// Task 5 will wire this to live bridge events over the Tauri event bus.
window.addEventListener("DOMContentLoaded", () => {
  const status = document.getElementById("status");
  if (status) {
    status.textContent = "starting…";
  }
});
