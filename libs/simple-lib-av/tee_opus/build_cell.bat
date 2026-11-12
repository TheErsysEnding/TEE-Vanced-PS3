@echo off
rem build_cell.bat : lib\libtee_opus.a with Sony's SDK (ppu-lv2-gcc on the PATH, or under C:\usr\local\cell):
rem tee_opus.c and libopus 1.5.2's decoder (the files of libopus_files.txt). Made by tools/make_opus_baustein.sh.
setlocal
set PATH=C:\usr\local\cell\host-win32\ppu\bin;C:\usr\local\cell\host-win32\bin;%PATH%
if exist obj rmdir /s /q obj
mkdir obj
if not exist lib mkdir lib
set F=-O2 -std=gnu99 -w -DOPUS_BUILD -DVAR_ARRAYS -DHAVE_LRINTF=1 -DHAVE_LRINT=1 -Ilibopus/include -Ilibopus/celt -Ilibopus/silk -Ilibopus/silk/float -Ilibopus -I.
ppu-lv2-gcc %F% -c libopus\celt\bands.c -o obj\celt_bands.o || goto :fail
ppu-lv2-gcc %F% -c libopus\celt\celt.c -o obj\celt_celt.o || goto :fail
ppu-lv2-gcc %F% -c libopus\celt\celt_decoder.c -o obj\celt_celt_decoder.o || goto :fail
ppu-lv2-gcc %F% -c libopus\celt\celt_encoder.c -o obj\celt_celt_encoder.o || goto :fail
ppu-lv2-gcc %F% -c libopus\celt\celt_lpc.c -o obj\celt_celt_lpc.o || goto :fail
ppu-lv2-gcc %F% -c libopus\celt\cwrs.c -o obj\celt_cwrs.o || goto :fail
ppu-lv2-gcc %F% -c libopus\celt\entcode.c -o obj\celt_entcode.o || goto :fail
ppu-lv2-gcc %F% -c libopus\celt\entdec.c -o obj\celt_entdec.o || goto :fail
ppu-lv2-gcc %F% -c libopus\celt\entenc.c -o obj\celt_entenc.o || goto :fail
ppu-lv2-gcc %F% -c libopus\celt\kiss_fft.c -o obj\celt_kiss_fft.o || goto :fail
ppu-lv2-gcc %F% -c libopus\celt\laplace.c -o obj\celt_laplace.o || goto :fail
ppu-lv2-gcc %F% -c libopus\celt\mathops.c -o obj\celt_mathops.o || goto :fail
ppu-lv2-gcc %F% -c libopus\celt\mdct.c -o obj\celt_mdct.o || goto :fail
ppu-lv2-gcc %F% -c libopus\celt\modes.c -o obj\celt_modes.o || goto :fail
ppu-lv2-gcc %F% -c libopus\celt\pitch.c -o obj\celt_pitch.o || goto :fail
ppu-lv2-gcc %F% -c libopus\celt\quant_bands.c -o obj\celt_quant_bands.o || goto :fail
ppu-lv2-gcc %F% -c libopus\celt\rate.c -o obj\celt_rate.o || goto :fail
ppu-lv2-gcc %F% -c libopus\celt\vq.c -o obj\celt_vq.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\A2NLSF.c -o obj\silk_A2NLSF.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\CNG.c -o obj\silk_CNG.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\HP_variable_cutoff.c -o obj\silk_HP_variable_cutoff.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\LPC_analysis_filter.c -o obj\silk_LPC_analysis_filter.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\LPC_fit.c -o obj\silk_LPC_fit.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\LPC_inv_pred_gain.c -o obj\silk_LPC_inv_pred_gain.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\LP_variable_cutoff.c -o obj\silk_LP_variable_cutoff.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\NLSF2A.c -o obj\silk_NLSF2A.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\NLSF_VQ.c -o obj\silk_NLSF_VQ.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\NLSF_VQ_weights_laroia.c -o obj\silk_NLSF_VQ_weights_laroia.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\NLSF_decode.c -o obj\silk_NLSF_decode.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\NLSF_del_dec_quant.c -o obj\silk_NLSF_del_dec_quant.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\NLSF_encode.c -o obj\silk_NLSF_encode.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\NLSF_stabilize.c -o obj\silk_NLSF_stabilize.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\NLSF_unpack.c -o obj\silk_NLSF_unpack.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\NSQ.c -o obj\silk_NSQ.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\NSQ_del_dec.c -o obj\silk_NSQ_del_dec.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\PLC.c -o obj\silk_PLC.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\VAD.c -o obj\silk_VAD.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\VQ_WMat_EC.c -o obj\silk_VQ_WMat_EC.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\ana_filt_bank_1.c -o obj\silk_ana_filt_bank_1.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\biquad_alt.c -o obj\silk_biquad_alt.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\bwexpander.c -o obj\silk_bwexpander.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\bwexpander_32.c -o obj\silk_bwexpander_32.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\check_control_input.c -o obj\silk_check_control_input.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\code_signs.c -o obj\silk_code_signs.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\control_SNR.c -o obj\silk_control_SNR.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\control_audio_bandwidth.c -o obj\silk_control_audio_bandwidth.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\control_codec.c -o obj\silk_control_codec.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\debug.c -o obj\silk_debug.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\dec_API.c -o obj\silk_dec_API.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\decode_core.c -o obj\silk_decode_core.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\decode_frame.c -o obj\silk_decode_frame.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\decode_indices.c -o obj\silk_decode_indices.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\decode_parameters.c -o obj\silk_decode_parameters.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\decode_pitch.c -o obj\silk_decode_pitch.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\decode_pulses.c -o obj\silk_decode_pulses.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\decoder_set_fs.c -o obj\silk_decoder_set_fs.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\enc_API.c -o obj\silk_enc_API.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\encode_indices.c -o obj\silk_encode_indices.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\encode_pulses.c -o obj\silk_encode_pulses.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\gain_quant.c -o obj\silk_gain_quant.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\init_decoder.c -o obj\silk_init_decoder.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\init_encoder.c -o obj\silk_init_encoder.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\inner_prod_aligned.c -o obj\silk_inner_prod_aligned.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\interpolate.c -o obj\silk_interpolate.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\lin2log.c -o obj\silk_lin2log.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\log2lin.c -o obj\silk_log2lin.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\pitch_est_tables.c -o obj\silk_pitch_est_tables.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\process_NLSFs.c -o obj\silk_process_NLSFs.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\quant_LTP_gains.c -o obj\silk_quant_LTP_gains.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\resampler.c -o obj\silk_resampler.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\resampler_down2.c -o obj\silk_resampler_down2.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\resampler_down2_3.c -o obj\silk_resampler_down2_3.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\resampler_private_AR2.c -o obj\silk_resampler_private_AR2.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\resampler_private_IIR_FIR.c -o obj\silk_resampler_private_IIR_FIR.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\resampler_private_down_FIR.c -o obj\silk_resampler_private_down_FIR.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\resampler_private_up2_HQ.c -o obj\silk_resampler_private_up2_HQ.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\resampler_rom.c -o obj\silk_resampler_rom.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\shell_coder.c -o obj\silk_shell_coder.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\sigm_Q15.c -o obj\silk_sigm_Q15.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\sort.c -o obj\silk_sort.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\stereo_LR_to_MS.c -o obj\silk_stereo_LR_to_MS.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\stereo_MS_to_LR.c -o obj\silk_stereo_MS_to_LR.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\stereo_decode_pred.c -o obj\silk_stereo_decode_pred.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\stereo_encode_pred.c -o obj\silk_stereo_encode_pred.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\stereo_find_predictor.c -o obj\silk_stereo_find_predictor.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\stereo_quant_pred.c -o obj\silk_stereo_quant_pred.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\sum_sqr_shift.c -o obj\silk_sum_sqr_shift.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\table_LSF_cos.c -o obj\silk_table_LSF_cos.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\tables_LTP.c -o obj\silk_tables_LTP.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\tables_NLSF_CB_NB_MB.c -o obj\silk_tables_NLSF_CB_NB_MB.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\tables_NLSF_CB_WB.c -o obj\silk_tables_NLSF_CB_WB.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\tables_gain.c -o obj\silk_tables_gain.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\tables_other.c -o obj\silk_tables_other.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\tables_pitch_lag.c -o obj\silk_tables_pitch_lag.o || goto :fail
ppu-lv2-gcc %F% -c libopus\silk\tables_pulses_per_block.c -o obj\silk_tables_pulses_per_block.o || goto :fail
ppu-lv2-gcc %F% -c libopus\src\extensions.c -o obj\src_extensions.o || goto :fail
ppu-lv2-gcc %F% -c libopus\src\opus.c -o obj\src_opus.o || goto :fail
ppu-lv2-gcc %F% -c libopus\src\opus_decoder.c -o obj\src_opus_decoder.o || goto :fail
ppu-lv2-gcc -O2 -std=gnu99 -Wall -Werror -Ilibopus/include -I. -c tee_opus.c -o obj\tee_opus.o || goto :fail
if exist lib\libtee_opus.a del lib\libtee_opus.a
ppu-lv2-ar rcs lib\libtee_opus.a obj\*.o || goto :fail
rmdir /s /q obj
echo lib\libtee_opus.a built
exit /b 0
:fail
echo BUILD FAILED
exit /b 1
