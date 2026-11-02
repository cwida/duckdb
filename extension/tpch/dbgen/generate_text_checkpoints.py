#!/usr/bin/env python3
# Generates include/dbgen/text_pool_checkpoints.h
#
# The TPC-H text pool is a fixed pseudo-text sequence produced by a sequential grammar walk over a single
# RNG stream. A checkpoint stores, for every chunk boundary, the start offset and RNG state of the sentence
# covering that boundary, so chunks can be generated independently and in parallel at runtime.
#
# This mirrors gen_sentence() in extension/tpch/dbgen/text.cpp; the C++ side verifies at runtime that every
# generated chunk ends exactly on the next checkpoint, so a mismatch here fails loudly.
import os
import re
import sys

CHUNK_SHIFT = 18  # 256 KiB, the size of a DuckDB memory block
CHUNK_COUNT = 300 * 1024 * 1024 >> CHUNK_SHIFT
TEXT_SEED = 933588178  # ctx->Seed[5], the text pregeneration stream

MODULUS = 2147483647
MULTIPLIER = 16807
D_M = 2147483647.0

ROOT = os.path.dirname(os.path.abspath(__file__))
DISTS_HEADER = os.path.join(ROOT, 'include', 'dbgen', 'dists_dss.h')
OUTPUT = os.path.join(ROOT, 'include', 'dbgen', 'text_pool_checkpoints.h')


def load_dists_text():
    with open(DISTS_HEADER) as f:
        source = f.read()
    fragments = re.findall(r'"((?:[^"\\]|\\.)*)"', source)
    return ''.join(fragments).encode('utf-8').decode('unicode_escape')


def read_dist(text, name):
    # mirrors read_dist() in bm_utils.cpp: returns (cumulative_weights, lengths)
    weights = []
    lengths = []
    total = 0
    in_dist = False
    for line in text.split('\n'):
        if '#' in line:
            line = line[: line.index('#')]
        if not line.strip():
            continue
        if not in_dist:
            tokens = line.split()
            if len(tokens) >= 2 and tokens[0].upper() == 'BEGIN' and tokens[1].upper() == name.upper():
                in_dist = True
            continue
        if line.split()[0].upper() == 'END':
            break
        token, weight = line.split('|', 1)
        weight = int(weight.strip())
        if token.upper() == 'COUNT':
            continue
        total += weight
        weights.append(total)
        lengths.append(len(token))
    if not weights:
        raise ValueError('distribution %s not found' % name)
    return weights, lengths


class Dist:
    def __init__(self, text, name):
        self.weights, self.lengths = read_dist(text, name)
        self.max = self.weights[-1]
        # weight -> length of the first entry whose cumulative weight reaches it (gen_index in text.cpp)
        self.index = [0] * (self.max + 1)
        i = 0
        for w in range(self.max + 1):
            while self.weights[i] < w:
                i += 1
            self.index[w] = self.lengths[i]


class Generator:
    def __init__(self, text):
        self.seed = TEXT_SEED
        self.nouns = Dist(text, 'nouns')
        self.verbs = Dist(text, 'verbs')
        self.adjectives = Dist(text, 'adjectives')
        self.adverbs = Dist(text, 'adverbs')
        self.auxillaries = Dist(text, 'auxillaries')
        self.terminators = Dist(text, 'terminators')
        self.prepositions = Dist(text, 'prepositions')
        self.grammar = Dist(text, 'grammar')
        self.np = Dist(text, 'np')
        self.vp = Dist(text, 'vp')

    def unif(self, low, high):
        self.seed = (self.seed * MULTIPLIER) % MODULUS
        return low + int((self.seed / D_M) * float(high - low + 1))

    def word(self, dist):
        # gen_text_index: word followed by a space
        return dist.index[self.unif(1, dist.max)] + 1

    def form(self, dist, form_count):
        j = self.unif(1, dist.max)
        return sum(1 for i in range(form_count) if dist.weights[i] < j)

    def gen_vp(self):
        index = self.form(self.vp, 3)
        if index == 0:
            return self.word(self.verbs)
        if index == 1:
            return self.word(self.auxillaries) + self.word(self.verbs)
        if index == 2:
            return self.word(self.verbs) + self.word(self.adverbs)
        return self.word(self.auxillaries) + self.word(self.verbs) + self.word(self.adverbs)

    def gen_np(self):
        index = self.form(self.np, 3)
        if index == 0:
            return self.word(self.nouns)
        if index == 1:
            return self.word(self.adjectives) + self.word(self.nouns)
        if index == 2:
            # "adjective, adjective noun "
            return self.word(self.adjectives) + 1 + self.word(self.adjectives) + self.word(self.nouns)
        return self.word(self.adverbs) + self.word(self.adjectives) + self.word(self.nouns)

    def gen_preposition(self):
        return self.word(self.prepositions) + 4 + self.gen_np()

    def gen_terminator(self):
        # gen_text overwrites the previous space with the terminator; the sentence ends with a space
        return self.terminators.index[self.unif(1, self.terminators.max)]

    def gen_sentence(self):
        index = self.form(self.grammar, 4)
        if index == 0:
            length = self.gen_np() + self.gen_vp()
        elif index == 1:
            length = self.gen_np() + self.gen_vp() + self.gen_preposition()
        elif index == 2:
            length = self.gen_np() + self.gen_vp() + self.gen_np()
        elif index == 3:
            length = self.gen_np() + self.gen_preposition() + self.gen_vp() + self.gen_np()
        else:
            length = self.gen_np() + self.gen_preposition() + self.gen_vp() + self.gen_preposition()
        return length + self.gen_terminator()


def main():
    generator = Generator(load_dists_text())
    checkpoints = []
    position = 0
    next_boundary = 0
    max_sentence = 0
    while len(checkpoints) <= CHUNK_COUNT:
        seed_before = generator.seed
        length = generator.gen_sentence()
        max_sentence = max(max_sentence, length)
        while len(checkpoints) <= CHUNK_COUNT and next_boundary < position + length:
            checkpoints.append((position, seed_before))
            next_boundary += 1 << CHUNK_SHIFT
        position += length
        if len(checkpoints) % 500 == 0 and next_boundary == len(checkpoints) << CHUNK_SHIFT:
            sys.stderr.write('\r%d / %d' % (len(checkpoints), CHUNK_COUNT))
    sys.stderr.write('\nlongest sentence: %d bytes\n' % max_sentence)

    with open(OUTPUT, 'w') as f:
        f.write('// Generated by extension/tpch/dbgen/generate_text_checkpoints.py - do not edit\n')
        f.write('#pragma once\n\n#include <stdint.h>\n\n')
        f.write('#define TEXT_POOL_CHUNK_SHIFT %d\n' % CHUNK_SHIFT)
        f.write('#define TEXT_POOL_CHUNK_COUNT %d\n' % CHUNK_COUNT)
        f.write('#define TEXT_POOL_MAX_SENTENCE_LEN %d\n\n' % max_sentence)
        f.write('struct text_pool_checkpoint {\n\tuint32_t offset;\n\tuint32_t seed;\n};\n\n')
        f.write(
            '// checkpoint k: start offset and RNG state of the sentence covering byte k << TEXT_POOL_CHUNK_SHIFT\n'
        )
        f.write('static const text_pool_checkpoint TEXT_POOL_CHECKPOINTS[TEXT_POOL_CHUNK_COUNT + 1] = {\n')
        for i in range(0, len(checkpoints), 4):
            row = ', '.join('{%d, %d}' % cp for cp in checkpoints[i : i + 4])
            f.write('    %s,\n' % row)
        f.write('};\n')


if __name__ == '__main__':
    main()
