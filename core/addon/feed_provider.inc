// The motion-vector shader behind the companion effect: which one AMD_Neural_Feed.fx samples, whether
// it is on, and whether it runs before the effect. Part of the core's unity build, included by
// neural.cpp before AdoptFeedEffect.

// The motion-vector shaders the companion effect can be compiled against, with the AMDNR_MV_PROVIDER
// each one is sampled under (effects/AMD_Neural_Feed.fx; anything but 1..4 is the texMotionVectors
// three). Name checks only: the effect binds the selected provider's output texture itself. This
// exists to answer "is anything actually writing that texture", which is the difference between a
// real field and a page of zeros the network would read as "nothing moved" -- a wrong answer, where
// no answer at all leaves the estimator in charge. tools/motion_feed_check.py holds the pairing to
// the effect's own blocks.
constexpr struct
{
    const char *file, *tech;
    int define;
} kMvProviders[] = {
    { "MartysMods_LAUNCHPAD.fx", "MartysMods_Launchpad", 1 },
    { "vort_Motion.fx", "vort_MotionEffects", 2 },
    { "lumenite_Kernel.fx", "Lumenite_Kernel", 3 },
    { "lumenite_QuantMotion.fx", "Lumenite_QuantMotion", 4 },
    { "qUINT_motionvectors.fx", "MotionVectors", 0 },
    { "dh_uber_motion.fx", "DH_UBER_MOTION_020", 0 },
    { "MotionEstimation.fx", "DRME", 0 },
};

// Whether a technique of that name exists and is ticked. ReShade keeps a technique whose effect
// failed to compile in its list and lets it be "enabled" -- it just never runs -- so this is
// necessary but not sufficient, and the guide probe is what catches the rest.
bool TechniqueOn(const char *file, const char *tech)
{
    if (g.effects == nullptr)
        return false;
    const auto t = g.effects->find_technique(file, tech);
    return t.handle != 0 && g.effects->get_technique_state(t);
}

// The provider the effect samples, as its block in AMD_Neural_Feed.fx picks it: the effect's own
// definition first, then the preset's global one, and 1 (Launchpad) when neither sets it.
int FeedProviderDefine()
{
    char value[16] {};
    if (!g.effects->get_preprocessor_definition_for_effect("AMD_Neural_Feed.fx", "AMDNR_MV_PROVIDER", value) &&
        !g.effects->get_preprocessor_definition("AMDNR_MV_PROVIDER", value))
        return 1;
    const int n = std::atoi(value);
    return n >= 1 && n <= 4 ? n : 0;
}

// Any provider being on used to be enough (issue #18): Launchpad ticked with the effect compiled for
// VORT samples a texture nothing writes, and the field arrives all zero while the panel says the effect
// is feeding it. Now the one the effect samples has to be ticked. Below the effect in the list it is
// still taken -- the effect reads last frame's field, a frame late, which setups have run on -- and
// AdoptFeedEffect says to move it above. Returns the provider that is ticked, or null; `below` says it
// runs after the effect.
const char *FeedProviderOn(bool &below)
{
    below = false;
    const int define = FeedProviderDefine();
    for (const auto &p : kMvProviders)
    {
        if (p.define != define || !TechniqueOn(p.file, p.tech))
            continue;
        const auto feed = g.effects->find_technique("AMD_Neural_Feed.fx", "AMD_Neural_Feed");
        const auto mine = g.effects->find_technique(p.file, p.tech);
        int seen = 0, feedAt = -1, mineAt = -1;
        g.effects->enumerate_techniques(nullptr, [&](reshade::api::effect_runtime *, reshade::api::effect_technique t) {
            feedAt = t.handle == feed.handle ? seen : feedAt;
            mineAt = t.handle == mine.handle ? seen : mineAt;
            ++seen;
        });
        below = feedAt >= 0 && mineAt > feedAt;
        return p.tech;
    }
    return nullptr;
}
