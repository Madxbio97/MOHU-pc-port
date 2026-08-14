#ifndef PSYX_GUEST_GTE_H
#define PSYX_GUEST_GTE_H

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct PsyXGuestGteProjection {
  uint32_t packed_sxy;
  float view_x;
  float view_y;
  float view_z;
  float projective_depth;
  float screen_x;
  float screen_y;
  float screen_h;
  float screen_offset_x;
  float screen_offset_y;
  uint64_t mesh_vertex_id;
  uint32_t exact_projection;
  uint32_t valid;
} PsyXGuestGteProjection;

/* Executes one guest RTPS/RTPT against a complete COP2 register snapshot.
 * Integer registers are updated exactly by PsyCross's GTE operator while the
 * unsaturated projection tuple is copied out before its transient PGXP cache
 * allocation is rewound. */
int PsyX_GteExecuteGuestProjection(uint32_t data[32], uint32_t control[32],
                                   uint32_t instruction,
                                   PsyXGuestGteProjection projections[3],
                                   uint32_t *projection_count);

#if defined(__cplusplus)
}
#endif

#endif
