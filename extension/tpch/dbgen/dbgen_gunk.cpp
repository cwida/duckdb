/*
 * Copyright owned by the Transaction Processing Performance Council.
 *
 * A copy of the license is included under extension/tpch/dbgen/LICENSE
 * in this repository.
 *
 * You may not use this file except in compliance with the License.
 *
 * THE TPC SOFTWARE IS AVAILABLE WITHOUT CHARGE FROM TPC.
 */
#include "dbgen/dbgen_gunk.hpp"

#include "dbgen/dss.h"
#include "dbgen/dsstypes.h"

void load_dists(long textBufferSize, DBGenContext *ctx) {
	read_dist("p_cntr", &p_cntr_set);
	read_dist("colors", &colors);
	read_dist("p_types", &p_types_set);
	read_dist("nations", &nations);
	read_dist("regions", &regions);
	read_dist("o_oprio", &o_priority_set);
	read_dist("instruct", &l_instruct_set);
	read_dist("smode", &l_smode_set);
	read_dist("category", &l_category_set);
	read_dist("rflag", &l_rflag_set);
	read_dist("msegmnt", &c_mseg_set);

	/* load the distributions that contain text generation */
	read_dist("nouns", &nouns);
	read_dist("verbs", &verbs);
	read_dist("adjectives", &adjectives);
	read_dist("adverbs", &adverbs);
	read_dist("auxillaries", &auxillaries);
	read_dist("terminators", &terminators);
	read_dist("articles", &articles);
	read_dist("prepositions", &prepositions);
	read_dist("grammar", &grammar);
	read_dist("np", &np);
	read_dist("vp", &vp);

	/* populate the text buffer used to generate random text */
	init_text_pool(textBufferSize, ctx);
}

static void cleanup_dist(distribution *target) {
	if (!target) {
		return;
	}
	if (target->list) {
		for (int i = 0; i < target->count; i++) {
			if (target->list[i].text) {
				free(target->list[i].text);
			}
		}
		free(target->list);
	}
}

void cleanup_dists(void) {
	cleanup_dist(&p_cntr_set);
	cleanup_dist(&colors);
	cleanup_dist(&p_types_set);
	cleanup_dist(&nations);
	cleanup_dist(&regions);
	cleanup_dist(&o_priority_set);
	cleanup_dist(&l_instruct_set);
	cleanup_dist(&l_smode_set);
	cleanup_dist(&l_category_set);
	cleanup_dist(&l_rflag_set);
	cleanup_dist(&c_mseg_set);
	cleanup_dist(&nouns);
	cleanup_dist(&verbs);
	cleanup_dist(&adjectives);
	cleanup_dist(&adverbs);
	cleanup_dist(&auxillaries);
	cleanup_dist(&terminators);
	cleanup_dist(&articles);
	cleanup_dist(&prepositions);
	cleanup_dist(&grammar);
	cleanup_dist(&np);
	cleanup_dist(&vp);

	free_text_pool();
}
