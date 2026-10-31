/*
 * thread_layout — OpenThread struct layouts for the thread overlay.
 *
 * GENERATED rows: do not edit them by hand.  The bundled OpenThread images
 * carry no DWARF (with it they are 28 MB); their rows come from DWARF builds
 * that are the same program (firmware/{cc2538dk,nrf52840-dk}/PROVENANCE.md:
 * the recipe, and tools/check-same-program.py), added with
 *
 *   tools/gen-struct-layout.py --update src/services/thread_layout.h \
 *       --name ot-cli-ftd.cc2538dk ~/work/ot-cc2538/build-dbg/bin/ot-cli-ftd
 *
 * The overlay reads OpenThread's one static instance (ot::gInstanceRaw, an
 * ot::Instance) and picks a layout by the image's machine and that symbol's
 * size; see struct_layout.h.
 *
 * layout-spec: Instance:mMle.mRole,mMle.mDeviceMode.mMode,mMle.mRloc16,mMle.mRouterId,mMle.mLeaderData.mPartitionId,mMle.mLeaderData.mLeaderRouterId,mMle.mParent.mRloc16,mMle.mParent.mState,mMle.mRouterTable.mRouters.mElements,mMle.mRouterTable.mRouters.mLength Router:mRloc16,mState,mLinkInfo.mLinkQualityIn,mLinkInfo.mLinkQualityOut */
#ifndef THREAD_LAYOUT_H
#define THREAD_LAYOUT_H

#include "struct_layout.h"

static const struct_layout_row_t thread_layout_rows[] = {
    /* rows:begin (tools/gen-struct-layout.py --update) */
    /* layout openthread-cc2538: ot-cli-ftd.cc2538dk */
    { "openthread-cc2538", 40, "Instance", "", 0, 12880, 0, 0 },
    { "openthread-cc2538", 40, "Instance", "mMle.mRole", 7361, 1, 0, 0 },
    { "openthread-cc2538", 40, "Instance", "mMle.mDeviceMode.mMode", 7363, 1, 0, 0 },
    { "openthread-cc2538", 40, "Instance", "mMle.mRloc16", 7364, 2, 0, 0 },
    { "openthread-cc2538", 40, "Instance", "mMle.mRouterId", 7993, 1, 0, 0 },
    { "openthread-cc2538", 40, "Instance", "mMle.mLeaderData.mPartitionId", 7392, 4, 0, 0 },
    { "openthread-cc2538", 40, "Instance", "mMle.mLeaderData.mLeaderRouterId", 7399, 1, 0, 0 },
    { "openthread-cc2538", 40, "Instance", "mMle.mParent.mRloc16", 7428, 2, 0, 0 },
    { "openthread-cc2538", 40, "Instance", "mMle.mParent.mState", 7430, 1, 0, 4 },
    { "openthread-cc2538", 40, "Instance", "mMle.mRouterTable.mRouters.mElements", 9764, 1664, 0, 0 },
    { "openthread-cc2538", 40, "Instance", "mMle.mRouterTable.mRouters.mLength", 11428, 1, 0, 0 },
    { "openthread-cc2538", 40, "Router", "", 0, 52, 0, 0 },
    { "openthread-cc2538", 40, "Router", "mRloc16", 28, 2, 0, 0 },
    { "openthread-cc2538", 40, "Router", "mState", 30, 1, 0, 4 },
    { "openthread-cc2538", 40, "Router", "mLinkInfo.mLinkQualityIn", 36, 1, 0, 2 },
    { "openthread-cc2538", 40, "Router", "mLinkInfo.mLinkQualityOut", 36, 1, 2, 2 },
    /* layout openthread-nrf52840: ot-cli-ftd.nrf52840-dk */
    { "openthread-nrf52840", 40, "Instance", "", 0, 33176, 0, 0 },
    { "openthread-nrf52840", 40, "Instance", "mMle.mRole", 22233, 1, 0, 0 },
    { "openthread-nrf52840", 40, "Instance", "mMle.mDeviceMode.mMode", 22235, 1, 0, 0 },
    { "openthread-nrf52840", 40, "Instance", "mMle.mRloc16", 22236, 2, 0, 0 },
    { "openthread-nrf52840", 40, "Instance", "mMle.mRouterId", 22865, 1, 0, 0 },
    { "openthread-nrf52840", 40, "Instance", "mMle.mLeaderData.mPartitionId", 22264, 4, 0, 0 },
    { "openthread-nrf52840", 40, "Instance", "mMle.mLeaderData.mLeaderRouterId", 22271, 1, 0, 0 },
    { "openthread-nrf52840", 40, "Instance", "mMle.mParent.mRloc16", 22300, 2, 0, 0 },
    { "openthread-nrf52840", 40, "Instance", "mMle.mParent.mState", 22302, 1, 0, 4 },
    { "openthread-nrf52840", 40, "Instance", "mMle.mRouterTable.mRouters.mElements", 29356, 1664, 0, 0 },
    { "openthread-nrf52840", 40, "Instance", "mMle.mRouterTable.mRouters.mLength", 31020, 1, 0, 0 },
    { "openthread-nrf52840", 40, "Router", "", 0, 52, 0, 0 },
    { "openthread-nrf52840", 40, "Router", "mRloc16", 28, 2, 0, 0 },
    { "openthread-nrf52840", 40, "Router", "mState", 30, 1, 0, 4 },
    { "openthread-nrf52840", 40, "Router", "mLinkInfo.mLinkQualityIn", 36, 1, 0, 2 },
    { "openthread-nrf52840", 40, "Router", "mLinkInfo.mLinkQualityOut", 36, 1, 2, 2 },
    /* rows:end */
};

#endif /* THREAD_LAYOUT_H */
