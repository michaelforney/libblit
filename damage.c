#include <stdlib.h>
#include <pixman.h>
#include "blt.h"

struct blt_damage {
	int num_regions, offset;
	struct pixman_region32 regions[];
};

static struct pixman_region32 infinite_damage = {
	.extents = {
		.x1 = INT32_MIN, .y1 = INT32_MIN,
		.x2 = INT32_MAX, .y2 = INT32_MAX,
	},
};

struct blt_damage *
blt_new_damage(int num_regions)
{
	struct blt_damage *damage;
	size_t i;

	damage = malloc(sizeof(*damage) + sizeof(damage->regions[0]) * num_regions);
	if (!damage)
		return NULL;
	damage->num_regions = num_regions;
	damage->offset = 0;
	for (i = 0; i < num_regions; ++i)
		pixman_region32_init_with_extents(&damage->regions[i], &infinite_damage.extents);
	return damage;
}

struct pixman_region32 *
blt_damage(struct blt_damage *damage, int age, struct pixman_region32 *new_damage)
{
	size_t i;

	if (new_damage) {
		for (i = 0; i < damage->num_regions; ++i)
			pixman_region32_union(&damage->regions[i], &damage->regions[i], new_damage);
	}
	if (age > damage->num_regions)
		return &infinite_damage;
	return &damage->regions[(age + damage->offset) % damage->num_regions];
}

void
blt_cycle_damage(struct blt_damage *damage)
{
	damage->offset = (damage->offset + damage->num_regions - 1) % damage->num_regions;
	pixman_region32_clear(&damage->regions[damage->offset]);
}
