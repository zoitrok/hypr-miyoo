#include "dsp/fft.h"

#include <math.h>

void fft_forward(float *re, float *im, int n)
{
    if (n < 2)
        return;

    /* Bit-reversal permutation. */
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;

        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }

    /* Danielson-Lanczos butterflies, doubling the block size each pass. */
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * M_PI / len;
        float wr_step = (float)cos(ang);
        float wi_step = (float)sin(ang);

        for (int i = 0; i < n; i += len) {
            float wr = 1.0f, wi = 0.0f;

            for (int k = 0; k < len / 2; k++) {
                int a = i + k;
                int b = a + len / 2;

                float tr = re[b] * wr - im[b] * wi;
                float ti = re[b] * wi + im[b] * wr;

                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;

                /* Recurrence rather than a table: n is small and this keeps
                 * the function allocation-free and self-contained. Error is
                 * well under what a 40-bar display can show. */
                float next_wr = wr * wr_step - wi * wi_step;
                wi = wr * wi_step + wi * wr_step;
                wr = next_wr;
            }
        }
    }
}
