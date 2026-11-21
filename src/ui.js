/*
 * Vial UI for Schwung
 *
 * Uses shared sound generator UI base for consistent preset browsing.
 *
 * GPL-3.0 License
 */

/* Shared utilities - absolute path for module location independence */
import { createSoundGeneratorUI } from '/data/UserData/schwung/shared/sound_generator_ui.mjs';

/* Create the UI with Vial customizations */
const ui = createSoundGeneratorUI({
    moduleName: 'Vial',

    onInit: (state) => {
        /* Vial-specific initialization */
    },

    onTick: (state) => {
        /* Vial-specific per-tick updates */
    },

    onPresetChange: (preset) => {
        /* Kill hanging notes on preset change */
        host_module_set_param('all_notes_off', '1');
    },

    showPolyphony: false,
    showOctave: true,
});

/* Export required callbacks */
globalThis.init = ui.init;
globalThis.tick = ui.tick;
globalThis.onMidiMessageInternal = ui.onMidiMessageInternal;
globalThis.onMidiMessageExternal = ui.onMidiMessageExternal;
