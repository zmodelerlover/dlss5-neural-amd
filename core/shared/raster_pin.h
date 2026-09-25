#pragma once
// When the network's raster follows a new back buffer size. EnsureResources asks on every present
// it is handed, on every route; tools/raster_pin_check.py compiles this and runs it.
//
// A new wanted size is followed once it has lasted kHold presents in a row AND is more than 2% off
// on some axis. Either test alone is wrong, because each re-raster re-stages the engine: Xenosaga 2
// walks 1080/1014/994/1008 lines every few frames (8% apart), and PCSX2 flaps between 974 and 971
// (0.3%). Held for ever, as it was, NFS windowed from 1080 to 1017 lines ran the network on a frame
// stretched to the old shape for the rest of the session.
// ponytail: kHold is the knob. Ceiling: a flapper that holds one size longer than kHold re-stages
// once per hold.
struct RasterPin
{
    static constexpr unsigned kHold = 120;
    unsigned w = 0, h = 0, streak = 0;  // the last wanted size, and the presents it has lasted

    // True to keep the raster at netW x netH rather than rebuild it at the wanted nw x nh.
    bool Keep(unsigned nw, unsigned nh, unsigned netW, unsigned netH)
    {
        streak = (nw == w && nh == h) ? streak + 1 : 0;
        w = nw;
        h = nh;
        if (nw == netW && nh == netH)
            return false;
        const bool slight = (nw > netW ? nw - netW : netW - nw) * 50 <= netW &&
                            (nh > netH ? nh - netH : netH - nh) * 50 <= netH;
        return slight || streak < kHold;
    }
};
