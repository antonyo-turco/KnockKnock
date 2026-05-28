/* Minimal stub for AvailabilityMacros.h
 * Some CMake/compiler detection tests include <AvailabilityMacros.h>
 * on macOS. Provide a minimal stub so cross-compiler detection succeeds
 * when the real macOS SDK headers are not available to the cross-compiler.
 */
#ifndef AVAILABILITYMACROS_H
#define AVAILABILITYMACROS_H

/* Define common availability macros as no-ops */
#ifndef __OSX_AVAILABLE_STARTING
#define __OSX_AVAILABLE_STARTING(_a,_b)
#endif

#ifndef __IPHONE_10_0
#define __IPHONE_10_0
#endif

#endif /* AVAILABILITYMACROS_H */
