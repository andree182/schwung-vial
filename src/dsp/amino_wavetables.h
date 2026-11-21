#ifndef AMINO_WAVETABLES_H
#define AMINO_WAVETABLES_H

#ifdef __cplusplus
extern "C" {
#endif

#define AMINO_NUM_TABLES 74
#define AMINO_NUM_FRAMES 64
#define AMINO_FRAME_LEN  128

extern const signed char amino_wavetable_data[AMINO_NUM_TABLES][AMINO_NUM_FRAMES][AMINO_FRAME_LEN];
extern const char *const amino_table_names[AMINO_NUM_TABLES];

#ifdef __cplusplus
}
#endif

#endif /* AMINO_WAVETABLES_H */
