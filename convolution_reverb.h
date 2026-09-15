#ifndef AUDIOPROCESS_CONVOLUTION_REVERB_H
#define AUDIOPROCESS_CONVOLUTION_REVERB_H

typedef struct ConvolutionReverb ConvolutionReverb;
#ifdef __cplusplus
extern "C" {
#endif

ConvolutionReverb *convolution_reverb_create(
        int sample_rate, float room_percent, float decay_seconds, float damping_percent);
void convolution_reverb_destroy(ConvolutionReverb *reverb);
void convolution_reverb_process(
        ConvolutionReverb *reverb, float input_l, float input_r, float *wet_l, float *wet_r);
#ifdef __cplusplus
}
#endif

#endif
