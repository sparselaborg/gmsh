#ifndef HXT_BOUNDARY_RECOVERY_H
#define HXT_BOUNDARY_RECOVERY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "hxt_mesh.h"
#include "hxt_surfaceModifications.h"


HXTStatus hxt_boundary_recovery(HXTMesh *mesh, double tol, HXTSurfMod **surfChange);

#ifdef __cplusplus
}
#endif

#endif
