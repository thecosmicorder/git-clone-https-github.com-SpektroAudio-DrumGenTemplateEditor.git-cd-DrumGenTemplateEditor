from pathlib import Path

p = Path('AuroraCloudscape/AuroraCloudscape.cpp')
s = p.read_text()
old = '''    hw.ProcessAllControls();
    const bool shift_pressed = hw.GetButton(SW_SHIFT).Pressed();

    if(hw.GetButton(SW_FREEZE).RisingEdge())
    {
        if(shift_pressed) lush_latched = !lush_latched;
        else freeze_latched = !freeze_latched;
    }
    if(hw.GetButton(SW_REVERSE).RisingEdge())
    {
        if(shift_pressed) filter_latched = !filter_latched;
        else rotate_latched = !rotate_latched;
    }
'''
new = '''    hw.ProcessAllControls();

    // v1.6 explicit four-effect chord router.
    // SHIFT is a modifier only. It never toggles an effect by itself.
    // A short grace window makes near-simultaneous SHIFT+button presses reliable.
    static float shift_grace_seconds = 0.0f;
    const bool shift_pressed = hw.GetButton(SW_SHIFT).Pressed();
    if(shift_pressed)
        shift_grace_seconds = 0.18f;
    else
    {
        shift_grace_seconds -= static_cast<float>(size) / sample_rate;
        if(shift_grace_seconds < 0.0f)
            shift_grace_seconds = 0.0f;
    }

    const bool modifier = shift_pressed || shift_grace_seconds > 0.0f;
    const bool freeze_edge = hw.GetButton(SW_FREEZE).RisingEdge();
    const bool reverse_edge = hw.GetButton(SW_REVERSE).RisingEdge();

    if(freeze_edge)
    {
        if(modifier)
        {
            lush_latched = !lush_latched;       // SHIFT + FREEZE = LUSH
            shift_grace_seconds = 0.0f;
        }
        else
        {
            freeze_latched = !freeze_latched;   // FREEZE = FREEZE
        }
    }

    if(reverse_edge)
    {
        if(modifier)
        {
            filter_latched = !filter_latched;   // SHIFT + REVERSE = FILTER DELAY
            shift_grace_seconds = 0.0f;
        }
        else
        {
            rotate_latched = !rotate_latched;   // REVERSE = ROTATE/PING-PONG
        }
    }
'''
if old not in s:
    raise SystemExit('Expected v1.5 button-routing block not found')
p.write_text(s.replace(old, new, 1))
