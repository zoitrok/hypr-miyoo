#ifndef HYPR_FFT_H
#define HYPR_FFT_H

/* In-place radix-2 complex FFT, power-of-two sizes.
 *
 * Deliberately the simple complex form rather than a packed real-input
 * transform. A real-input FFT would halve the work, but at 512 points and 60
 * frames a second this costs a few thousand operations per frame -- far below
 * anything that matters next to the rendering -- and the straightforward
 * version has much less room to be subtly wrong. */
void fft_forward(float *re, float *im, int n);

#endif
