Per-monitor fractional scaling on X11, carried as a patch against the release.

The same feature as `../qt6-base`, with the same origin: https://github.com/dec05eba/qtbase
(dev branch, 6.13.0-alpha1 at the time of writing), commits 88875e18..f1f2aa13.
Qt 5.15 is not what that branch is written against, so this is a port rather than
a copy: the Qt6 diff was taken as the specification and each change rewritten
against the Qt5 API, in the same shape the Qt6 patch has.

The two are not interchangeable and the port is not mechanical. The differences
that mattered:

- Qt5 has no `QWindowPrivate::devicePixelRatio` cache and no
  `updateDevicePixelRatio()`, so the `handleScaleChange()` split does not exist
  here; Qt5 notifies windows directly from
  `processScreenLogicalDotsPerInchChange()` instead.
- No `std::optional` (this tree is not built as C++17): the factor is a
  `bool` plus a `qreal` on `QPlatformScreenPrivate`, with an explicit
  `clearHighDpiScaleFactor()`, because the RandR property can disappear again
  and the screen has to fall back to the screen-wide logic when it does.
- No `qxp::function_ref` (`std::function`), no `QList::removeIf` (index loop),
  no `QFlags::testAnyFlags` (`& mask != 0`), no `QBasicTimer` chrono overload
  (`start(int, QObject*)`), no `QTimerEvent::id()` (`timerId()`).
- Qt5 builds the xcb plugin with qmake, so the new `qxcbwindow_scale.cpp` is
  registered in `xcb_qpa_lib.pro` rather than in a CMakeLists.txt.
- Qt5 has no `QXcbScreen::setMonitor()` and no `updateColorSpaceAndEdid()`;
  `updateHighDpiScaleFactor()` is called from the constructor and from
  `setOutput()`, which is where Qt5 learns an output's name and mode.
- `QPlatformScreen::screen()` is hidden inside `QXcbScreen` by its
  `xcb_screen_t *screen()`, so the one call that wants the QScreen is qualified.

The feature itself: a compositor cannot scale X11 clients, so the toolkit has to
do it. The server publishes the intended scale of each output as the RandR
output property "DPI" (`RR_PROPERTY_DPI`, an XA_INTEGER created for every
output); this reads it per output, turns it into a per-screen scale factor
instead of one screen-wide one derived from the physical size, and rescales a
window when it is moved to an output with a different DPI.

Two departures from the Qt6 patch. The first is a consequence of the port; the
second is a bug the Qt6 patch had too, fixed in both:

- a factor can be withdrawn, not only set. Qt6 gets that from `std::optional`,
  which Qt5 does not have here, so the factor is a `bool` plus a `qreal` and
  `clearHighDpiScaleFactor()` is called when the property is gone. Without it
  the stale factor stayed and the "unchanged" comparison could never match
  again, so every later `setOutput()` re-ran a full rescale.
- `QHighDpiScaling::m_screenFactorSet` is recomputed each pass instead of being
  latched, with a separate flag for a factor that really was configured through
  `setScreenFactor()` / `QT_SCREEN_SCALE_FACTORS`. A latched flag kept scaling
  active with a factor nothing published any more and left `m_active` true for
  the rest of the session.

Regenerate from a fork that carries the Qt6 version:

    git -C <qt6 fork clone> diff 3f81ad0f93..HEAD      # the specification
    <port by hand; verify with `git apply --check` against the KDE tree>

The patch applies to the tree Arch's qt5-base PKGBUILD builds, which is KDE's
qtbase at 46aa5835798404f6608c9c3b881819ee10c5d351 (5.15.19+kde+r97), not to
qt/qtbase's 5.15 branch. Check against that commit before bumping.
