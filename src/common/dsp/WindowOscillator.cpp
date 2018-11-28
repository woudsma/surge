//-------------------------------------------------------------------------------------------------------
//	Copyright 2006 Claes Johanson & Vember Audio
//-------------------------------------------------------------------------------------------------------
#include "Oscillator.h"
#if !MAC && !__linux__
#include <intrin.h>
#endif
#ifdef __linux__
#include <stdint.h>
#endif

/* wt2 osc */

WindowOscillator::WindowOscillator(SurgeStorage* storage,
                                   OscillatorStorage* oscdata,
                                   pdata* localcopy)
    : Oscillator(storage, oscdata, localcopy)
{}

void WindowOscillator::init(float pitch, bool is_display)
{
   memset(&Sub, 0, sizeof(Sub));

   ActiveSubOscs = limit_range(oscdata->p[6].val.i, 1, wt2_suboscs - 1);
   if (is_display)
      ActiveSubOscs = 1;

   float out_attenuation_inv = sqrt((float)ActiveSubOscs);
   OutAttenuation = 1.0f / (out_attenuation_inv * 16777216.f);

   if (ActiveSubOscs == 1)
   {
      DetuneBias = 1;
      DetuneOffset = 0;

      Sub.Gain[0][0] = 128;
      Sub.Gain[0][1] = 128; // unity gain
      Sub.Pos[0] = (storage->WindowWT.size << 16);
   }
   else
   {
      DetuneBias = (float)2.f / ((float)ActiveSubOscs - 1.f);
      DetuneOffset = -1.f;

      bool odd = ActiveSubOscs & 1;
      float mid = ActiveSubOscs * 0.5 - 0.5;
      int half = ActiveSubOscs >> 1;
      for (int i = 0; i < ActiveSubOscs; i++)
      {
         float d = fabs((float)i - mid) / mid;
         if (odd && (i >= half))
            d = -d;
         if (i & 1)
            d = -d;

         Sub.Gain[i][0] = limit_range((int)(float)(128.f * megapanL(d)), 0, 255);
         Sub.Gain[i][1] = limit_range((int)(float)(128.f * megapanR(d)), 0, 255);

         if (oscdata->retrigger.val.b)
            Sub.Pos[i] = (storage->WindowWT.size + ((storage->WindowWT.size * i) / ActiveSubOscs))
                         << 16;
         else
            Sub.Pos[i] = (storage->WindowWT.size + (rand() & (storage->WindowWT.size - 1))) << 16;
      }
   }
}

WindowOscillator::~WindowOscillator()
{}

void WindowOscillator::init_ctrltypes()
{
   oscdata->p[0].set_name("Shape");
   oscdata->p[0].set_type(ct_percent);
   oscdata->p[1].set_name("Formant");
   oscdata->p[1].set_type(ct_pitch);
   oscdata->p[2].set_name("Window");
   oscdata->p[2].set_type(ct_wt2window);

   oscdata->p[5].set_name("Uni Spread");
   oscdata->p[5].set_type(ct_oscspread);
   oscdata->p[6].set_name("Uni Count");
   oscdata->p[6].set_type(ct_osccountWT);
}
void WindowOscillator::init_default_values()
{
   oscdata->p[0].val.f = 0.0f;
   oscdata->p[1].val.f = 0.0f;
   oscdata->p[2].val.i = 0;
   oscdata->p[5].val.f = 0.2f;
   oscdata->p[6].val.i = 1;
}

__forceinline unsigned int BigMULr16(unsigned int a, unsigned int b)
{
   // 64-bit unsigned multiply with right shift by 16 bits
#if PPC
   return (__mulhw(a, b) << 16) | ((a * b) >> 16);
#elif _M_X64
   unsigned __int64 c = __emulu(a, b);
   return c >> 16;
#elif __linux__
   uint64_t c = (uint64_t)a * (uint64_t)b;
   return c >> 16;
#else
   unsigned int result;
   __asm
   {
		mov eax,a
		mov ecx,b
		mul ecx
		shl edx, 16
		shr eax, 16
		or eax,edx
         // TODO fixa return för gcc asm
		mov result, eax
   }
   return result;
#endif
}

#if MAC || __linux__
__forceinline bool _BitScanReverse(unsigned long* result, unsigned long bits)
{
   *result = __builtin_ctz(bits);
   return true;
}
#endif

void WindowOscillator::ProcessSubOscs(bool stereo)
{
   const unsigned int M0Mask = 0x07f8;
   unsigned int SizeMask = (oscdata->wt.size << 16) - 1;
   unsigned int SizeMaskWin = (storage->WindowWT.size << 16) - 1;
   unsigned int WindowVsWavePO2 = storage->WindowWT.size_po2 - oscdata->wt.size_po2;
   unsigned char Window = limit_range(oscdata->p[2].val.i, 0, 8);

   int Table = limit_range(
       (int)(float)(oscdata->wt.n_tables * localcopy[oscdata->p[0].param_id_in_scene].f), 0,
       (int)oscdata->wt.n_tables - 1);
   int FormantMul =
       (int)(float)(65536.f * note_to_pitch(localcopy[oscdata->p[1].param_id_in_scene].f));
   FormantMul = max(FormantMul >> WindowVsWavePO2, 1);
#if PPC
   // Altivec path
   for (unsigned int so = 0; so < ActiveSubOscs; so++)
   {
      unsigned int Pos = Sub.Pos[so];
      unsigned int RatioA = Sub.Ratio[so];
      unsigned int MipMapA = 0;
      unsigned int MipMapB = 0;
      if (Sub.Table[so] >= oscdata->wt.n_tables)
         Sub.Table[so] = Table; // TableID kanske inte ‰r valid l‰ngre om en ny wavetable laddats

      unsigned long MSBpos;
      unsigned int bs = BigMULr16(RatioA, 3 * FormantMul);
      if (_BitScanReverse(&MSBpos, bs))
         MipMapB = limit_range((int)MSBpos - 17, 0, oscdata->wt.size_po2 - 1);

      if (_BitScanReverse(&MSBpos, 3 * RatioA))
         MipMapA = limit_range((int)MSBpos - 17, 0, storage->WindowWT.size_po2 - 1);

      short* WaveAdr = oscdata->wt.TableI16WeakPointers[MipMapB][Sub.Table[so]];
      short* WinAdr = storage->WindowWT.TableI16WeakPointers[MipMapA][Window];

      vSInt32 i0 = vec_splat_s32(0);

      for (int i = 0; i < block_size_os; i++)
      {
         Pos += RatioA;
         if (Pos & ~SizeMaskWin)
         {
            Sub.FormantMul[so] = FormantMul;
            Sub.Table[so] = Table;
            WaveAdr = oscdata->wt.TableI16WeakPointers[MipMapB][Table];
            Pos = Pos & SizeMaskWin;
         }

         unsigned int WinPos = Pos >> (16 + MipMapA);
         unsigned int WinSPos = (Pos >> (4 + MipMapA)) & 0xFF0;

         unsigned int FPos = BigMULr16(Sub.FormantMul[so], Pos) & SizeMask;

         unsigned int MPos = FPos >> (16 + MipMapB);
         unsigned int MSPos = ((FPos >> (4 + MipMapB)) & 0xFF0);

         short* WLp = &WaveAdr[MPos];
         vSInt16 WL = vec_perm(vec_ld(0, WLp), vec_ld(15, WLp), vec_lvsl(0, WLp));
         vSInt32 Wave = vec_msum(vec_ld(MSPos, sinctableI16), WL, i0);

         short* WiLp = &WinAdr[WinPos];
         vSInt16 WiL = vec_perm(vec_ld(0, WiLp), vec_ld(15, WiLp), vec_lvsl(0, WiLp));
         vSInt32 Win = vec_msum(vec_ld(WinSPos, sinctableI16), WiL, i0);

         // Sum
         int iWin, iWave, s4[4];
         vec_st(vec_sums(Win, i0), 0, s4);
         iWin = s4[3];
         vec_st(vec_sums(Wave, i0), 0, s4);
         iWave = s4[3];

         if (stereo)
         {
            int Out = ((iWin >> 13) * (iWave >> 13)) >> 7;
            IOutputL[i] += (Out * (int)Sub.Gain[so][0]) >> 6;
            IOutputR[i] += (Out * (int)Sub.Gain[so][1]) >> 6;
         }
         else
            IOutputL[i] += ((iWin >> 13) * (iWave >> 13)) >> 6;
      }
      Sub.Pos[so] = Pos;
   }
#else
#if (!_M_X64 && !MAC)
   if (SSE_VERSION >= 2)
#endif
   {
      // SSE2 path
      for (int so = 0; so < ActiveSubOscs; so++)
      {
         unsigned int Pos = Sub.Pos[so];
         unsigned int RatioA = Sub.Ratio[so];
         unsigned int MipMapA = 0;
         unsigned int MipMapB = 0;
         if (Sub.Table[so] >= oscdata->wt.n_tables)
            Sub.Table[so] = Table; // TableID kanske inte ar valid langre om en ny wavetable laddats

         unsigned long MSBpos;
         unsigned int bs = BigMULr16(RatioA, 3 * FormantMul);
         if (_BitScanReverse(&MSBpos, bs))
            MipMapB = limit_range((int)MSBpos - 17, 0, oscdata->wt.size_po2 - 1);

         if (_BitScanReverse(&MSBpos, 3 * RatioA))
            MipMapA = limit_range((int)MSBpos - 17, 0, storage->WindowWT.size_po2 - 1);

         short* WaveAdr = oscdata->wt.TableI16WeakPointers[MipMapB][Sub.Table[so]];
         short* WinAdr = storage->WindowWT.TableI16WeakPointers[MipMapA][Window];

         for (int i = 0; i < block_size_os; i++)
         {
            Pos += RatioA;
            if (Pos & ~SizeMaskWin)
            {
               Sub.FormantMul[so] = FormantMul;
               Sub.Table[so] = Table;
               WaveAdr = oscdata->wt.TableI16WeakPointers[MipMapB][Table];
               Pos = Pos & SizeMaskWin;
            }

            unsigned int WinPos = Pos >> (16 + MipMapA);
            unsigned int WinSPos = (Pos >> (8 + MipMapA)) & 0xFF;

            unsigned int FPos = BigMULr16(Sub.FormantMul[so], Pos) & SizeMask;

            unsigned int MPos = FPos >> (16 + MipMapB);
            unsigned int MSPos = ((FPos >> (8 + MipMapB)) & 0xFF);

            __m128i Wave = _mm_madd_epi16(_mm_load_si128(((__m128i*)sinctableI16 + MSPos)),
                                          _mm_loadu_si128((__m128i*)&WaveAdr[MPos]));

            __m128i Win = _mm_madd_epi16(_mm_load_si128(((__m128i*)sinctableI16 + WinSPos)),
                                         _mm_loadu_si128((__m128i*)&WinAdr[WinPos]));

            // Sum
            _MM_ALIGN16 int iWin[4], iWave[4];
#if MAC
            // this should be very fast on C2D/C1D (and there are no macs with K8's)
            iWin[0] = _mm_cvtsi128_si32(Win);
            iWin[1] = _mm_cvtsi128_si32(_mm_shuffle_epi32(Win, _MM_SHUFFLE(1, 1, 1, 1)));
            iWin[2] = _mm_cvtsi128_si32(_mm_shuffle_epi32(Win, _MM_SHUFFLE(2, 2, 2, 2)));
            iWin[3] = _mm_cvtsi128_si32(_mm_shuffle_epi32(Win, _MM_SHUFFLE(3, 3, 3, 3)));
            iWave[0] = _mm_cvtsi128_si32(Wave);
            iWave[1] = _mm_cvtsi128_si32(_mm_shuffle_epi32(Wave, _MM_SHUFFLE(1, 1, 1, 1)));
            iWave[2] = _mm_cvtsi128_si32(_mm_shuffle_epi32(Wave, _MM_SHUFFLE(2, 2, 2, 2)));
            iWave[3] = _mm_cvtsi128_si32(_mm_shuffle_epi32(Wave, _MM_SHUFFLE(3, 3, 3, 3)));
#else
            _mm_store_si128((__m128i*)&iWin, Win);
            _mm_store_si128((__m128i*)&iWave, Wave);
#endif

            iWin[0] = (iWin[0] + iWin[1] + iWin[2] + iWin[3]) >> 13;
            iWave[0] = (iWave[0] + iWave[1] + iWave[2] + iWave[3]) >> 13;
            if (stereo)
            {
               int Out = (iWin[0] * iWave[0]) >> 7;
               IOutputL[i] += (Out * (int)Sub.Gain[so][0]) >> 6;
               IOutputR[i] += (Out * (int)Sub.Gain[so][1]) >> 6;
            }
            else
               IOutputL[i] += (iWin[0] * iWave[0]) >> 6;
         }
         Sub.Pos[so] = Pos;
      }
   }
#if (!_M_X64 && !MAC)
   else
   {
      // MMX path
      for (unsigned int so = 0; so < ActiveSubOscs; so++)
      {
         unsigned int Pos = Sub.Pos[so];
         unsigned int RatioA = Sub.Ratio[so];
         unsigned int MipMapA = 0;
         unsigned int MipMapB = 0;
         if (Sub.Table[so] >= oscdata->wt.n_tables)
            Sub.Table[so] = Table; // TableID kanske inte ‰r valid l‰ngre om en ny wavetable laddats

         unsigned long MSBpos;

         unsigned int bs = BigMULr16(RatioA, 3 * FormantMul);
         if (_BitScanReverse(&MSBpos, bs))
            MipMapB = limit_range((int)MSBpos - 17, 0, oscdata->wt.size_po2 - 1);

         if (_BitScanReverse(&MSBpos, 3 * RatioA))
            MipMapA = limit_range((int)MSBpos - 17, 0, storage->WindowWT.size_po2 - 1);

         short* WaveAdr = oscdata->wt.TableI16WeakPointers[MipMapB][Sub.Table[so]];
         short* WinAdr = storage->WindowWT.TableI16WeakPointers[MipMapA][Window];

         for (int i = 0; i < block_size_os; i++)
         {
            Pos += RatioA;
            if (Pos & ~SizeMaskWin)
            {
               Sub.FormantMul[so] = FormantMul;
               Sub.Table[so] = Table;
               WaveAdr = oscdata->wt.TableI16WeakPointers[MipMapB][Table];
               Pos = Pos & SizeMaskWin;
            }

            unsigned int WinPos = Pos >> (16 + MipMapA);
            unsigned int WinSPos = (Pos >> (8 + MipMapA)) & 0xFF;

            unsigned int FPos = BigMULr16(Sub.FormantMul[so], Pos) & SizeMask;

            unsigned int MPos = FPos >> (16 + MipMapB);
            unsigned int MSPos = ((FPos >> (8 + MipMapB)) & 0xFF);

            __m64* WaveSincPtr = (__m64*)sinctableI16 + (MSPos << 1);
            __m64* WavePtr = (__m64*)&WaveAdr[MPos];
            __m64 Wave = _mm_add_pi32(_mm_madd_pi16(*WaveSincPtr, *WavePtr),
                                      _mm_madd_pi16(*(WaveSincPtr + 1), *(WavePtr + 1)));

            __m64* WinSincPtr = (__m64*)sinctableI16 + (WinSPos << 1);
            __m64* WinPtr = (__m64*)&WinAdr[WinPos];
            __m64 Win = _mm_add_pi32(_mm_madd_pi16(*WinSincPtr, *WinPtr),
                                     _mm_madd_pi16(*(WinSincPtr + 1), *(WinPtr + 1)));

            // Sum
            int iWin, iWave;

            iWin = (*(int*)&Win + *((int*)&Win + 1)) >> 13;
            iWave = (*(int*)&Wave + *((int*)&Wave + 1)) >> 13;

            if (stereo)
            {
               int Out = (iWin * iWave) >> 7;
               IOutputL[i] += (Out * (int)Sub.Gain[so][0]) >> 6;
               IOutputR[i] += (Out * (int)Sub.Gain[so][1]) >> 6;
            }
            else
               IOutputL[i] += (iWin * iWave) >> 6;
         }
         Sub.Pos[so] = Pos;
      }
      _mm_empty();
   }
#endif
#endif
}

void WindowOscillator::process_block(float pitch, float drift, bool stereo, bool FM, float fmdepth)
{
   memset(IOutputL, 0, block_size_os * sizeof(int));
   if (stereo)
      memset(IOutputR, 0, block_size_os * sizeof(int));

   float Detune = localcopy[oscdata->p[5].param_id_in_scene].f;
   for (int l = 0; l < ActiveSubOscs; l++)
   {
      Sub.DriftLFO[l][0] = drift_noise(Sub.DriftLFO[l][1]);
      float f = note_to_pitch((pitch - 57.f) + drift * Sub.DriftLFO[l][0] +
                              Detune * (DetuneOffset + DetuneBias * (float)l));
      int Ratio = Float2Int(220.f * 32768.f * f * (float)(storage->WindowWT.size) *
                            samplerate_inv); // (65536.f*0.5f), 0.5 fˆr oversampling
      Sub.Ratio[l] = Ratio;
   }

   ProcessSubOscs(stereo);

   // TODO idÈ: coupla tvÂ subvoices sÂ dom kan alternera window med varandra
   // ( hÂll window i 180-phasediff)
   // borde ge bra formant-trixning utan mellanrum
   // kan ha en parameter som fadar vikten av den alternerande mellan -1 till +1

   // TODO idÈ: free-run mode d‰r en omstart av fˆnstret inte Âterstartar oscillatorn?

   // int32 -> float conversion
#if PPC
   vFloat scale = (vFloat)OutAttenuation;
   const vFloat m0 = (vFloat)0.f;

   if (stereo)
   {
      for (int i = 0; i < block_size_os; i += 8)
      {
         vFloat a = vec_ctf(vec_ld(0, &IOutputL[i]), 0);
         vFloat b = vec_ctf(vec_ld(0, &IOutputL[i + 4]), 0);
         vFloat c = vec_ctf(vec_ld(0, &IOutputR[i]), 0);
         vFloat d = vec_ctf(vec_ld(0, &IOutputR[i + 4]), 0);
         vec_st(vec_madd(a, scale, m0), 0, &output[i]);
         vec_st(vec_madd(b, scale, m0), 0, &output[i + 4]);
         vec_st(vec_madd(c, scale, m0), 0, &outputR[i]);
         vec_st(vec_madd(d, scale, m0), 0, &outputR[i + 4]);
      }
   }
   else
   {
      for (int i = 0; i < block_size_os; i += 8)
      {
         vFloat a = vec_ctf(vec_ld(0, &IOutputL[i]), 0);
         vFloat b = vec_ctf(vec_ld(0, &IOutputL[i + 4]), 0);
         vec_st(vec_madd(a, scale, m0), 0, &output[i]);
         vec_st(vec_madd(b, scale, m0), 0, &output[i + 4]);
      }
   }
#else
   __m128 scale = _mm_load1_ps(&OutAttenuation);
#if (!_M_X64 && !MAC)
   if (SSE_VERSION >= 2)
#endif
   {
      // SSE2 path
      if (stereo)
      {
         for (int i = 0; i < block_size_os; i += 4)
         {
            _mm_store_ps(&output[i], _mm_mul_ps(_mm_cvtepi32_ps(*(__m128i*)&IOutputL[i]), scale));
            _mm_store_ps(&outputR[i], _mm_mul_ps(_mm_cvtepi32_ps(*(__m128i*)&IOutputR[i]), scale));
         }
      }
      else
      {
         for (int i = 0; i < block_size_os; i += 4)
         {
            _mm_store_ps(&output[i], _mm_mul_ps(_mm_cvtepi32_ps(*(__m128i*)&IOutputL[i]), scale));
         }
      }
   }
#if (!_M_X64 && !MAC)
   else
   {
      // MMX/SSE1 path
      if (stereo)
      {
         for (int i = 0; i < block_size_os; i += 4)
         {
            _mm_store_ps(&output[i], _mm_mul_ps(_mm_cvtpi32x2_ps(*(__m64*)&IOutputL[i],
                                                                 *(__m64*)&IOutputL[i + 2]),
                                                scale));
            _mm_store_ps(&outputR[i], _mm_mul_ps(_mm_cvtpi32x2_ps(*(__m64*)&IOutputR[i],
                                                                  *(__m64*)&IOutputR[i + 2]),
                                                 scale));
         }
      }
      else
      {
         for (int i = 0; i < block_size_os; i += 4)
         {
            _mm_store_ps(&output[i], _mm_mul_ps(_mm_cvtpi32x2_ps(*(__m64*)&IOutputL[i],
                                                                 *(__m64*)&IOutputL[i + 2]),
                                                scale));
         }
      }
      _mm_empty();
   }
#endif

#endif
}