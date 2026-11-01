// Compatibility shim. The Wi-Fi download UI moved to the stepper in wififlow.js (#wifiDlg), which polls
// state.ap itself. status.js still calls renderApCard() on every STATUS, and a phone can hold a cached
// status.js for ~10 min after a deploy, so the export stays (a no-op) until a later deploy drops the call.
export function renderApCard(){}
