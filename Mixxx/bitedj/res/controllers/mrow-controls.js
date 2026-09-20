// Same unshifted browse behavior as Pioneer-DDJ-FLX4-script.js.
// Independent of FLX4 initialization: works even without that controller attached.
var MROWControls = {};
MROWControls.init = function() {};
MROWControls.shutdown = function() {};
MROWControls.browseRotate = function(_channel, _control, value) {
    if (value === 0) {
        return;
    }
    if (engine.getValue("[Tab]", "current") === 0) {
        script.triggerControl("[Channel1]",
            value === 0x7f ? "waveform_zoom_up" : "waveform_zoom_down", 100);
    } else {
        engine.setValue("[Library]", "MoveVertical", value > 0x40 ? value - 0x80 : value);
    }
};
MROWControls.browsePress = function(_channel, _control, value) {
    if (value === 0) {
        return;
    }
    if (engine.getValue("[Tab]", "current") === 0) {
        engine.setValue("[Tab]", "current", 1);
        engine.setValue("[Tab]", "library", 1);
    } else {
        script.triggerControl("[Library]", "MoveFocusForward", 100);
    }
};
