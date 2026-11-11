#ifndef ELDEN_PAYLOAD_LZ4_H
#define ELDEN_PAYLOAD_LZ4_H

#define ELDEN_PACKED_MAGIC       "ELDLZ401"
#define ELDEN_PACKED_CHUNK       65536U
#define ELDEN_PACKED_MAX_VOLUME  (256U * 1024U * 1024U)

// A raw LZ4 block has one token per literal/match pair. A nibble of 15
// extends the length with bytes, ending at the first byte below 255.
STATIC BOOLEAN
EldenLz4Length (
  CONST UINT8  **Source,
  CONST UINT8  *End,
  UINTN        *Length
  )
{
  UINT8  Extra;

  if (*Length == 15) {
    do {
      if ((*Source == End) || (*Length > ELDEN_PACKED_CHUNK)) {
        return FALSE;
      }
      Extra = *(*Source)++;
      *Length += Extra;
    } while (Extra == 255);
  }
  return TRUE;
}

STATIC BOOLEAN
EldenLz4Decode (
  CONST UINT8  *Source,
  UINTN        SourceSize,
  UINT8        *Output,
  UINTN        OutputSize
  )
{
  CONST UINT8  *SourceEnd = Source + SourceSize;
  UINT8        *OutputEnd = Output + OutputSize;
  UINT8        *Next = Output;

  while (Source < SourceEnd) {
    UINT8  Token = *Source++;
    UINTN  Literals = Token >> 4;
    UINTN  Match = Token & 15;
    UINTN  Distance;
    UINT8  *From;

    if (!EldenLz4Length (&Source, SourceEnd, &Literals) ||
        (Literals > (UINTN)(SourceEnd - Source)) ||
        (Literals > (UINTN)(OutputEnd - Next))) {
      return FALSE;
    }
    CopyMem (Next, Source, Literals);
    Next += Literals;
    Source += Literals;
    if (Source == SourceEnd) {
      return Next == OutputEnd;
    }
    if ((SourceEnd - Source) < 2) {
      return FALSE;
    }
    Distance = Source[0] | ((UINTN)Source[1] << 8);
    Source += 2;
    if ((Distance == 0) || (Distance > (UINTN)(Next - Output)) ||
        !EldenLz4Length (&Source, SourceEnd, &Match) ||
        (Match + 4 > (UINTN)(OutputEnd - Next))) {
      return FALSE;
    }
    From = Next - Distance;
    for (Match += 4; Match != 0; Match--) {
      *Next++ = *From++;
    }
  }
  return FALSE;
}

#endif
