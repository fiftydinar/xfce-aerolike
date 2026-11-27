Per-monitor fractional scaling on X11, carried as a patch against the release.

Source: https://github.com/dec05eba/qtbase (dev, 6.13.0-alpha1 at the time of
writing), commits 88875e18..f1f2aa13, minus the two that were reverted upstream
in that branch (_X_WINDOW_DPI). Regenerate with:

    git -C <fork clone> diff 3f81ad0f93..HEAD > qt6-base-dpi.patch

Applied on top of the released version rather than building the fork: the diff
applies cleanly to 6.12.1, and a development snapshot of QtCore/QtGui would have
to match every other qt6-* package in the image, which the release does not.

The feature itself: a compositor cannot scale X11 clients, so the toolkit has to
do it. The server publishes the intended scale of each output as the RandR
output property "DPI" (RR_PROPERTY_DPI); this reads it per output, turns it into
a per-screen scale factor instead of one screen-wide one derived from the
physical size, and rescales a window when it is moved to an output with a
different DPI.

One fix on top of the fork's commits: QHighDpiScaling::m_screenFactorSet was
latched. The factor is read from the compositor and the property can go away -
output unplugged, property deleted, compositor restarted - but once the flag was
set nothing ever cleared it, so scaling stayed active with a factor nothing
published any more and m_active stayed true for the rest of the session.
Recomputed each pass now, with a separate flag for a factor that really was
configured through setScreenFactor() / QT_SCREEN_SCALE_FACTORS, which does
persist.
