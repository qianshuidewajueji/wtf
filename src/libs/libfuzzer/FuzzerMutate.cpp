//===- FuzzerMutate.cpp - Mutate a test input -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// Mutate a test input.
//===----------------------------------------------------------------------===//

#include "FuzzerMutate.h"
#include "FuzzerExtFunctions.h"
#include <cstdarg>

namespace fuzzer {

// Cross Data1 and Data2, store the result (up to MaxOutSize bytes) in Out.
size_t MutationDispatcher::CrossOver(const uint8_t *Data1, size_t Size1,
                                     const uint8_t *Data2, size_t Size2,
                                     uint8_t *Out, size_t MaxOutSize) {
  assert(Size1 || Size2);
  MaxOutSize = Rand(MaxOutSize) + 1;
  size_t OutPos = 0;
  size_t Pos1 = 0;
  size_t Pos2 = 0;
  size_t *InPos = &Pos1;
  size_t InSize = Size1;
  const uint8_t *Data = Data1;
  bool CurrentlyUsingFirstData = true;
  while (OutPos < MaxOutSize && (Pos1 < Size1 || Pos2 < Size2)) {
    // Merge a part of Data into Out.
    size_t OutSizeLeft = MaxOutSize - OutPos;
    if (*InPos < InSize) {
      size_t InSizeLeft = InSize - *InPos;
      size_t MaxExtraSize = std::min(OutSizeLeft, InSizeLeft);
      size_t ExtraSize = Rand(MaxExtraSize) + 1;
      memcpy(Out + OutPos, Data + *InPos, ExtraSize);
      OutPos += ExtraSize;
      (*InPos) += ExtraSize;
    }
    // Use the other input data on the next iteration.
    InPos = CurrentlyUsingFirstData ? &Pos2 : &Pos1;
    InSize = CurrentlyUsingFirstData ? Size2 : Size1;
    Data = CurrentlyUsingFirstData ? Data2 : Data1;
    CurrentlyUsingFirstData = !CurrentlyUsingFirstData;
  }
  return OutPos;
}

const void *SearchMemory(const void *Data, size_t DataLen, const void *Patt,
                         size_t PattLen) {
  // TODO: make this implementation more efficient.
  const char *Cdata = (const char *)Data;
  const char *Cpatt = (const char *)Patt;

  if (!Data || !Patt || DataLen == 0 || PattLen == 0 || DataLen < PattLen)
    return NULL;

  if (PattLen == 1)
    return memchr(Data, *Cpatt, DataLen);

  const char *End = Cdata + DataLen - PattLen + 1;

  for (const char *It = Cdata; It < End; ++It)
    if (It[0] == Cpatt[0] && memcmp(It, Cpatt, PattLen) == 0)
      return It;

  return NULL;
}

#ifdef __linux__
inline uint8_t Bswap(uint8_t x) { return x; }
inline uint16_t Bswap(uint16_t x) { return __builtin_bswap16(x); }
inline uint32_t Bswap(uint32_t x) { return __builtin_bswap32(x); }
inline uint64_t Bswap(uint64_t x) { return __builtin_bswap64(x); }
#else
inline uint8_t Bswap(uint8_t x) { return x; }
// Use alternatives to __builtin functions from <stdlib.h> and <intrin.h> on
// Windows since the builtins are not supported by MSVC.
inline uint16_t Bswap(uint16_t x) { return _byteswap_ushort(x); }
inline uint32_t Bswap(uint32_t x) { return _byteswap_ulong(x); }
inline uint64_t Bswap(uint64_t x) { return _byteswap_uint64(x); }
#endif

void Printf(const char *Fmt, ...) {
  static FILE *OutputFile = stderr;
  va_list ap;
  va_start(ap, Fmt);
  vfprintf(OutputFile, Fmt, ap);
  va_end(ap);
  fflush(OutputFile);
}

void PrintHexArray(const uint8_t *Data, size_t Size, const char *PrintAfter) {
  for (size_t i = 0; i < Size; i++)
    Printf("0x%x,", (unsigned)Data[i]);
  Printf("%s", PrintAfter);
}

void Print(const Unit &v, const char *PrintAfter) {
  PrintHexArray(v.data(), v.size(), PrintAfter);
}

void PrintASCIIByte(uint8_t Byte) {
  if (Byte == '\\')
    Printf("\\\\");
  else if (Byte == '"')
    Printf("\\\"");
  else if (Byte >= 32 && Byte < 127)
    Printf("%c", Byte);
  else
    Printf("\\x%02x", Byte);
}

void PrintASCII(const uint8_t *Data, size_t Size, const char *PrintAfter) {
  for (size_t i = 0; i < Size; i++)
    PrintASCIIByte(Data[i]);
  Printf("%s", PrintAfter);
}

void PrintASCII(const Unit &U, const char *PrintAfter) {
  PrintASCII(U.data(), U.size(), PrintAfter);
}

bool ToASCII(uint8_t *Data, size_t Size) {
  bool Changed = false;
  for (size_t i = 0; i < Size; i++) {
    uint8_t &X = Data[i];
    auto NewX = X;
    NewX &= 127;
    if (!isspace(NewX) && !isprint(NewX))
      NewX = ' ';
    Changed |= NewX != X;
    X = NewX;
  }
  return Changed;
}

static bool ParseDictionaryFile(const std::string &Text, Vector<Unit> *Units) {
  if (Text.empty()) {
    Printf("ParseDictionaryFile: file does not exist or is empty\n");
    return false;
  }
  std::istringstream ISS(Text);
  Units->clear();
  Unit U;
  int LineNo = 0;
  std::string S;
  while (std::getline(ISS, S, '\n')) {
    LineNo++;
    size_t Pos = 0;
    while (Pos < S.size() && isspace(S[Pos]))
      Pos++; // Skip spaces.
    if (Pos == S.size())
      continue; // Empty line.
    if (S[Pos] == '#')
      continue; // Comment line.
    if (ParseOneDictionaryEntry(S, &U)) {
      Units->push_back(U);
    } else {
      Printf("ParseDictionaryFile: error in line %d\n\t\t%s\n", LineNo,
             S.c_str());
      return false;
    }
  }
  return true;
}
static std::string FileToString(const std::string &Path) {
  std::ifstream T(Path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(T)),
                     std::istreambuf_iterator<char>());
}

static bool ParseOneDictionaryEntry(const std::string &Str, Unit *U) {
  U->clear();
  if (Str.empty())
    return false;
  size_t L = 0, R = Str.size() - 1; // We are parsing the range [L,R].
  // Skip spaces from both sides.
  while (L < R && isspace(Str[L]))
    L++;
  while (R > L && isspace(Str[R]))
    R--;
  if (R - L < 2)
    return false;
  // Check the closing "
  if (Str[R] != '"')
    return false;
  R--;
  // Find the opening "
  while (L < R && Str[L] != '"')
    L++;
  if (L >= R)
    return false;
  assert(Str[L] == '\"');
  L++;
  assert(L <= R);
  for (size_t Pos = L; Pos <= R; Pos++) {
    uint8_t V = (uint8_t)Str[Pos];
    if (!isprint(V) && !isspace(V))
      return false;
    if (V == '\\') {
      // Handle '\\'
      if (Pos + 1 <= R && (Str[Pos + 1] == '\\' || Str[Pos + 1] == '"')) {
        U->push_back(Str[Pos + 1]);
        Pos++;
        continue;
      }
      // Handle '\xAB'
      if (Pos + 3 <= R && Str[Pos + 1] == 'x' && isxdigit(Str[Pos + 2]) &&
          isxdigit(Str[Pos + 3])) {
        char Hex[] = "0xAA";
        Hex[2] = Str[Pos + 2];
        Hex[3] = Str[Pos + 3];
        U->push_back(strtol(Hex, nullptr, 16));
        Pos += 3;
        continue;
      }
      return false; // Invalid escape.
    } else {
      // Any other character.
      U->push_back(V);
    }
  }
  return true;
}

const size_t Dictionary::kMaxDictSize;

static void PrintASCII(const Word &W, const char *PrintAfter) {
  PrintASCII(W.data(), W.size(), PrintAfter);
}

MutationDispatcher::MutationDispatcher(Random &Rand,
                                       const FuzzingOptions &Options)
    : Rand(Rand), Options(Options) {
  DefaultMutators.insert(
      DefaultMutators.begin(),
      {
          {&MutationDispatcher::Mutate_EraseBytes, "EraseBytes"},
          {&MutationDispatcher::Mutate_InsertByte, "InsertByte"},
          {&MutationDispatcher::Mutate_InsertRepeatedBytes,
           "InsertRepeatedBytes"},
          {&MutationDispatcher::Mutate_ChangeByte, "ChangeByte"},
          {&MutationDispatcher::Mutate_ChangeBit, "ChangeBit"},
          {&MutationDispatcher::Mutate_ShuffleBytes, "ShuffleBytes"},
          {&MutationDispatcher::Mutate_ChangeASCIIInteger, "ChangeASCIIInt"},
          {&MutationDispatcher::Mutate_ChangeBinaryInteger, "ChangeBinInt"},
          {&MutationDispatcher::Mutate_CopyPart, "CopyPart"},
          {&MutationDispatcher::Mutate_CrossOver, "CrossOver"},
          {&MutationDispatcher::Mutate_AddWordFromManualDictionary,
           "ManualDict"},
          {&MutationDispatcher::Mutate_AddWordFromPersistentAutoDictionary,
           "PersAutoDict"},
      });
  if (Options.UseCmp)
    DefaultMutators.push_back(
        {&MutationDispatcher::Mutate_AddWordFromTORC, "CMP"});

  if (EF->LLVMFuzzerCustomMutator)
    Mutators.push_back({&MutationDispatcher::Mutate_Custom, "Custom"});
  else
    Mutators = DefaultMutators;

  if (EF->LLVMFuzzerCustomCrossOver)
    Mutators.push_back(
        {&MutationDispatcher::Mutate_CustomCrossOver, "CustomCrossOver"});

  Vector<Unit> Dictionary;
  if (ParseDictionaryFile(FileToString(Options.Dict), &Dictionary)) {
    for (auto &U : Dictionary) {
      if (U.size() <= Word::GetMaxSize())
        this->AddWordToManualDictionary(Word(U.data(), U.size()));
    }
  }
}

static char RandCh(Random &Rand) {
  if (Rand.RandBool())
    return char(Rand(256));
  const char Special[] = "!*'();:@&=+$,/?%#[]012Az-`~.\xff\x00";
  return Special[Rand(sizeof(Special) - 1)];
}

size_t MutationDispatcher::Mutate_Custom(uint8_t *Data, size_t Size,
                                         size_t MaxSize) {
  return EF->LLVMFuzzerCustomMutator(Data, Size, MaxSize,
                                     (unsigned int)Rand.Rand());
}

size_t MutationDispatcher::Mutate_CustomCrossOver(uint8_t *Data, size_t Size,
                                                  size_t MaxSize) {
  if (Size == 0)
    return 0;
  if (!CrossOverWith)
    return 0;
  const Unit &Other = *CrossOverWith;
  if (Other.empty())
    return 0;
  CustomCrossOverInPlaceHere.resize(MaxSize);
  auto &U = CustomCrossOverInPlaceHere;
  size_t NewSize = EF->LLVMFuzzerCustomCrossOver(
      Data, Size, Other.data(), Other.size(), U.data(), U.size(),
      (unsigned int)Rand.Rand());
  if (!NewSize)
    return 0;
  assert(NewSize <= MaxSize && "CustomCrossOver returned overisized unit");
  memcpy(Data, U.data(), NewSize);
  return NewSize;
}

size_t MutationDispatcher::Mutate_ShuffleBytes(uint8_t *Data, size_t Size,
                                               size_t MaxSize) {
  if (Size > MaxSize || Size == 0)
    return 0;
  size_t ShuffleAmount =
      Rand(std::min(Size, (size_t)8)) + 1; // [1,8] and <= Size.
  size_t ShuffleStart = Rand(Size - ShuffleAmount);
  assert(ShuffleStart + ShuffleAmount <= Size);
  std::shuffle(Data + ShuffleStart, Data + ShuffleStart + ShuffleAmount, Rand);
  return Size;
}

size_t MutationDispatcher::Mutate_EraseBytes(uint8_t *Data, size_t Size,
                                             size_t MaxSize) {
  if (Size <= 1)
    return 0;
  size_t N = Rand(Size / 2) + 1;
  assert(N < Size);
  size_t Idx = Rand(Size - N + 1);
  // Erase Data[Idx:Idx+N].
  memmove(Data + Idx, Data + Idx + N, Size - Idx - N);
  // Printf("Erase: %zd %zd => %zd; Idx %zd\n", N, Size, Size - N, Idx);
  return Size - N;
}

size_t MutationDispatcher::Mutate_InsertByte(uint8_t *Data, size_t Size,
                                             size_t MaxSize) {
  if (Size >= MaxSize)
    return 0;
  size_t Idx = Rand(Size + 1);
  // Insert new value at Data[Idx].
  memmove(Data + Idx + 1, Data + Idx, Size - Idx);
  Data[Idx] = RandCh(Rand);
  return Size + 1;
}

size_t MutationDispatcher::Mutate_InsertRepeatedBytes(uint8_t *Data,
                                                      size_t Size,
                                                      size_t MaxSize) {
  const size_t kMinBytesToInsert = 3;
  if (Size + kMinBytesToInsert >= MaxSize)
    return 0;
  size_t MaxBytesToInsert = std::min(MaxSize - Size, (size_t)128);
  size_t N = Rand(MaxBytesToInsert - kMinBytesToInsert + 1) + kMinBytesToInsert;
  assert(Size + N <= MaxSize && N);
  size_t Idx = Rand(Size + 1);
  // Insert new values at Data[Idx].
  memmove(Data + Idx + N, Data + Idx, Size - Idx);
  // Give preference to 0x00 and 0xff.
  uint8_t Byte =
      Rand.RandBool() ? uint8_t(Rand(256)) : (Rand.RandBool() ? 0 : 255);
  for (size_t i = 0; i < N; i++)
    Data[Idx + i] = Byte;
  return Size + N;
}

size_t MutationDispatcher::Mutate_ChangeByte(uint8_t *Data, size_t Size,
                                             size_t MaxSize) {
  if (Size > MaxSize)
    return 0;
  size_t Idx = Rand(Size);
  Data[Idx] = RandCh(Rand);
  return Size;
}

size_t MutationDispatcher::Mutate_ChangeBit(uint8_t *Data, size_t Size,
                                            size_t MaxSize) {
  if (Size > MaxSize)
    return 0;
  size_t Idx = Rand(Size);
  Data[Idx] ^= 1 << Rand(8);
  return Size;
}

size_t MutationDispatcher::Mutate_AddWordFromManualDictionary(uint8_t *Data,
                                                              size_t Size,
                                                              size_t MaxSize) {
  return AddWordFromDictionary(ManualDictionary, Data, Size, MaxSize);
}

size_t MutationDispatcher::ApplyDictionaryEntry(uint8_t *Data, size_t Size,
                                                size_t MaxSize,
                                                DictionaryEntry &DE) {
  const Word &W = DE.GetW();
  bool UsePositionHint = DE.HasPositionHint() &&
                         DE.GetPositionHint() + W.size() < Size &&
                         Rand.RandBool();
  if (Rand.RandBool()) { // Insert W.
    if (Size + W.size() > MaxSize)
      return 0;
    size_t Idx = UsePositionHint ? DE.GetPositionHint() : Rand(Size + 1);
    memmove(Data + Idx + W.size(), Data + Idx, Size - Idx);
    memcpy(Data + Idx, W.data(), W.size());
    Size += W.size();
  } else { // Overwrite some bytes with W.
    if (W.size() > Size)
      return 0;
    size_t Idx = UsePositionHint ? DE.GetPositionHint() : Rand(Size - W.size());
    memcpy(Data + Idx, W.data(), W.size());
  }
  return Size;
}

// Somewhere in the past we have observed a comparison instructions
// with arguments Arg1 Arg2. This function tries to guess a dictionary
// entry that will satisfy that comparison.
// It first tries to find one of the arguments (possibly swapped) in the
// input and if it succeeds it creates a DE with a position hint.
// Otherwise it creates a DE with one of the arguments w/o a position hint.
DictionaryEntry MutationDispatcher::MakeDictionaryEntryFromCMP(
    const void *Arg1, const void *Arg2, const void *Arg1Mutation,
    const void *Arg2Mutation, size_t ArgSize, const uint8_t *Data,
    size_t Size) {
  bool HandleFirst = Rand.RandBool();
  const void *ExistingBytes, *DesiredBytes;
  Word W;
  const uint8_t *End = Data + Size;
  for (int Arg = 0; Arg < 2; Arg++) {
    ExistingBytes = HandleFirst ? Arg1 : Arg2;
    DesiredBytes = HandleFirst ? Arg2Mutation : Arg1Mutation;
    HandleFirst = !HandleFirst;
    W.Set(reinterpret_cast<const uint8_t *>(DesiredBytes), uint8_t(ArgSize));
    const size_t kMaxNumPositions = 8;
    size_t Positions[kMaxNumPositions];
    size_t NumPositions = 0;
    for (const uint8_t *Cur = Data;
         Cur < End && NumPositions < kMaxNumPositions; Cur++) {
      Cur =
          (const uint8_t *)SearchMemory(Cur, End - Cur, ExistingBytes, ArgSize);
      if (!Cur)
        break;
      Positions[NumPositions++] = Cur - Data;
    }
    if (!NumPositions)
      continue;
    return DictionaryEntry(W, Positions[Rand(NumPositions)]);
  }
  DictionaryEntry DE(W);
  return DE;
}

template <class T>
DictionaryEntry MutationDispatcher::MakeDictionaryEntryFromCMP(
    T Arg1, T Arg2, const uint8_t *Data, size_t Size) {
  if (Rand.RandBool())
    Arg1 = Bswap(Arg1);
  if (Rand.RandBool())
    Arg2 = Bswap(Arg2);
  T Arg1Mutation = Arg1 + Rand(-1, 1);
  T Arg2Mutation = Arg2 + Rand(-1, 1);
  return MakeDictionaryEntryFromCMP(&Arg1, &Arg2, &Arg1Mutation, &Arg2Mutation,
                                    sizeof(Arg1), Data, Size);
}

DictionaryEntry MutationDispatcher::MakeDictionaryEntryFromCMP(
    const Word &Arg1, const Word &Arg2, const uint8_t *Data, size_t Size) {
  return MakeDictionaryEntryFromCMP(Arg1.data(), Arg2.data(), Arg1.data(),
                                    Arg2.data(), Arg1.size(), Data, Size);
}

size_t MutationDispatcher::Mutate_AddWordFromTORC(uint8_t *Data, size_t Size,
                                                  size_t MaxSize) {
  return 0;
#if 0
        Word W;
        DictionaryEntry DE;
        switch (Rand(4)) {
        case 0: {
            auto X = TPC.TORC8.Get(Rand.Rand());
            DE = MakeDictionaryEntryFromCMP(X.A, X.B, Data, Size);
        } break;
        case 1: {
            auto X = TPC.TORC4.Get(Rand.Rand());
            if ((X.A >> 16) == 0 && (X.B >> 16) == 0 && Rand.RandBool())
                DE = MakeDictionaryEntryFromCMP((uint16_t)X.A, (uint16_t)X.B, Data, Size);
            else
                DE = MakeDictionaryEntryFromCMP(X.A, X.B, Data, Size);
        } break;
        case 2: {
            auto X = TPC.TORCW.Get(Rand.Rand());
            DE = MakeDictionaryEntryFromCMP(X.A, X.B, Data, Size);
        } break;
        case 3: if (Options.UseMemmem) {
            auto X = TPC.MMT.Get(Rand.Rand());
            DE = DictionaryEntry(X);
        } break;
        default:
            assert(0);
        }
        if (!DE.GetW().size()) return 0;
        Size = ApplyDictionaryEntry(Data, Size, MaxSize, DE);
        if (!Size) return 0;
        DictionaryEntry& DERef =
            CmpDictionaryEntriesDeque[CmpDictionaryEntriesDequeIdx++ %
            kCmpDictionaryEntriesDequeSize];
        DERef = DE;
        CurrentDictionaryEntrySequence.push_back(&DERef);
        return Size;
#endif
}

size_t MutationDispatcher::Mutate_AddWordFromPersistentAutoDictionary(
    uint8_t *Data, size_t Size, size_t MaxSize) {
  return AddWordFromDictionary(PersistentAutoDictionary, Data, Size, MaxSize);
}

size_t MutationDispatcher::AddWordFromDictionary(Dictionary &D, uint8_t *Data,
                                                 size_t Size, size_t MaxSize) {
  if (Size > MaxSize)
    return 0;
  if (D.empty())
    return 0;
  DictionaryEntry &DE = D[Rand(D.size())];
  Size = ApplyDictionaryEntry(Data, Size, MaxSize, DE);
  if (!Size)
    return 0;
  DE.IncUseCount();
  CurrentDictionaryEntrySequence.push_back(&DE);
  return Size;
}

// Overwrites part of To[0,ToSize) with a part of From[0,FromSize).
// Returns ToSize.
size_t MutationDispatcher::CopyPartOf(const uint8_t *From, size_t FromSize,
                                      uint8_t *To, size_t ToSize) {
  // Copy From[FromBeg, FromBeg + CopySize) into To[ToBeg, ToBeg + CopySize).
  size_t ToBeg = Rand(ToSize);
  size_t CopySize = Rand(ToSize - ToBeg) + 1;
  assert(ToBeg + CopySize <= ToSize);
  CopySize = std::min(CopySize, FromSize);
  size_t FromBeg = Rand(FromSize - CopySize + 1);
  assert(FromBeg + CopySize <= FromSize);
  memmove(To + ToBeg, From + FromBeg, CopySize);
  return ToSize;
}

// Inserts part of From[0,ToSize) into To.
// Returns new size of To on success or 0 on failure.
size_t MutationDispatcher::InsertPartOf(const uint8_t *From, size_t FromSize,
                                        uint8_t *To, size_t ToSize,
                                        size_t MaxToSize) {
  if (ToSize >= MaxToSize)
    return 0;
  size_t AvailableSpace = MaxToSize - ToSize;
  size_t MaxCopySize = std::min(AvailableSpace, FromSize);
  size_t CopySize = Rand(MaxCopySize) + 1;
  size_t FromBeg = Rand(FromSize - CopySize + 1);
  assert(FromBeg + CopySize <= FromSize);
  size_t ToInsertPos = Rand(ToSize + 1);
  assert(ToInsertPos + CopySize <= MaxToSize);
  size_t TailSize = ToSize - ToInsertPos;
  if (To == From) {
    MutateInPlaceHere.resize(MaxToSize);
    memcpy(MutateInPlaceHere.data(), From + FromBeg, CopySize);
    memmove(To + ToInsertPos + CopySize, To + ToInsertPos, TailSize);
    memmove(To + ToInsertPos, MutateInPlaceHere.data(), CopySize);
  } else {
    memmove(To + ToInsertPos + CopySize, To + ToInsertPos, TailSize);
    memmove(To + ToInsertPos, From + FromBeg, CopySize);
  }
  return ToSize + CopySize;
}

size_t MutationDispatcher::Mutate_CopyPart(uint8_t *Data, size_t Size,
                                           size_t MaxSize) {
  if (Size > MaxSize || Size == 0)
    return 0;
  // If Size == MaxSize, `InsertPartOf(...)` will
  // fail so there's no point using it in this case.
  if (Size == MaxSize || Rand.RandBool())
    return CopyPartOf(Data, Size, Data, Size);
  else
    return InsertPartOf(Data, Size, Data, Size, MaxSize);
}

size_t MutationDispatcher::Mutate_ChangeASCIIInteger(uint8_t *Data, size_t Size,
                                                     size_t MaxSize) {
  if (Size > MaxSize)
    return 0;
  size_t B = Rand(Size);
  while (B < Size && !isdigit(Data[B]))
    B++;
  if (B == Size)
    return 0;
  size_t E = B;
  while (E < Size && isdigit(Data[E]))
    E++;
  assert(B < E);
  // now we have digits in [B, E).
  // strtol and friends don't accept non-zero-teminated data, parse it manually.
  uint64_t Val = Data[B] - '0';
  for (size_t i = B + 1; i < E; i++)
    Val = Val * 10 + Data[i] - '0';

  // Mutate the integer value.
  switch (Rand(5)) {
  case 0:
    Val++;
    break;
  case 1:
    Val--;
    break;
  case 2:
    Val /= 2;
    break;
  case 3:
    Val *= 2;
    break;
  case 4:
    Val = Rand(Val * Val);
    break;
  default:
    assert(0);
  }
  // Just replace the bytes with the new ones, don't bother moving bytes.
  for (size_t i = B; i < E; i++) {
    size_t Idx = E + B - i - 1;
    assert(Idx >= B && Idx < E);
    Data[Idx] = (Val % 10) + '0';
    Val /= 10;
  }
  return Size;
}

template <class T>
size_t ChangeBinaryInteger(uint8_t *Data, size_t Size, Random &Rand) {
  if (Size < sizeof(T))
    return 0;
  size_t Off = Rand(Size - sizeof(T) + 1);
  assert(Off + sizeof(T) <= Size);
  T Val;
  if (Off < 64 && !Rand(4)) {
    Val = decltype(Val)(Size);
    if (Rand.RandBool())
      Val = Bswap(Val);
  } else {
    memcpy(&Val, Data + Off, sizeof(Val));
    T Add = T(Rand(21));
    Add -= 10;
    if (Rand.RandBool())
      Val = Bswap(T(Bswap(Val) + Add)); // Add assuming different endiannes.
    else
      Val = Val + Add;               // Add assuming current endiannes.
    if (Add == 0 || Rand.RandBool()) // Maybe negate.
      Val = ~Val;
  }
  memcpy(Data + Off, &Val, sizeof(Val));
  return Size;
}

size_t MutationDispatcher::Mutate_ChangeBinaryInteger(uint8_t *Data,
                                                      size_t Size,
                                                      size_t MaxSize) {
  if (Size > MaxSize)
    return 0;
  switch (Rand(4)) {
  case 3:
    return ChangeBinaryInteger<uint64_t>(Data, Size, Rand);
  case 2:
    return ChangeBinaryInteger<uint32_t>(Data, Size, Rand);
  case 1:
    return ChangeBinaryInteger<uint16_t>(Data, Size, Rand);
  case 0:
    return ChangeBinaryInteger<uint8_t>(Data, Size, Rand);
  default:
    assert(0);
  }
  return 0;
}

size_t MutationDispatcher::Mutate_CrossOver(uint8_t *Data, size_t Size,
                                            size_t MaxSize) {
  if (Size > MaxSize)
    return 0;
  if (Size == 0)
    return 0;
  if (!CrossOverWith)
    return 0;
  const Unit &O = *CrossOverWith;
  if (O.empty())
    return 0;
  MutateInPlaceHere.resize(MaxSize);
  auto &U = MutateInPlaceHere;
  size_t NewSize = 0;
  switch (Rand(3)) {
  case 0:
    NewSize = CrossOver(Data, Size, O.data(), O.size(), U.data(), U.size());
    break;
  case 1:
    NewSize = InsertPartOf(O.data(), O.size(), U.data(), U.size(), MaxSize);
    if (!NewSize)
      NewSize = CopyPartOf(O.data(), O.size(), U.data(), U.size());
    break;
  case 2:
    NewSize = CopyPartOf(O.data(), O.size(), U.data(), U.size());
    break;
  default:
    assert(0);
  }
  assert(NewSize > 0 && "CrossOver returned empty unit");
  assert(NewSize <= MaxSize && "CrossOver returned overisized unit");
  memcpy(Data, U.data(), NewSize);
  return NewSize;
}

void MutationDispatcher::StartMutationSequence() {
  CurrentMutatorSequence.clear();
  CurrentDictionaryEntrySequence.clear();
}

// Copy successful dictionary entries to PersistentAutoDictionary.
void MutationDispatcher::RecordSuccessfulMutationSequence() {
  for (auto DE : CurrentDictionaryEntrySequence) {
    // PersistentAutoDictionary.AddWithSuccessCountOne(DE);
    DE->IncSuccessCount();
    assert(DE->GetW().size());
    // Linear search is fine here as this happens seldom.
    if (!PersistentAutoDictionary.ContainsWord(DE->GetW()))
      PersistentAutoDictionary.push_back({DE->GetW(), 1});
  }
}

void MutationDispatcher::PrintRecommendedDictionary() {
  Vector<DictionaryEntry> V;
  for (auto &DE : PersistentAutoDictionary)
    if (!ManualDictionary.ContainsWord(DE.GetW()))
      V.push_back(DE);
  if (V.empty())
    return;
  Printf("###### Recommended dictionary. ######\n");
  for (auto &DE : V) {
    assert(DE.GetW().size());
    Printf("\"");
    PrintASCII(DE.GetW(), "\"");
    Printf(" # Uses: %zd\n", DE.GetUseCount());
  }
  Printf("###### End of recommended dictionary. ######\n");
}

void MutationDispatcher::PrintMutationSequence() {
  Printf("MS: %zd ", CurrentMutatorSequence.size());
  for (auto M : CurrentMutatorSequence)
    Printf("%s-", M.Name);
  if (!CurrentDictionaryEntrySequence.empty()) {
    Printf(" DE: ");
    for (auto DE : CurrentDictionaryEntrySequence) {
      Printf("\"");
      PrintASCII(DE->GetW(), "\"-");
    }
  }
}

size_t MutationDispatcher::Mutate(uint8_t *Data, size_t Size, size_t MaxSize) {
  return MutateImpl(Data, Size, MaxSize, Mutators);
}

size_t MutationDispatcher::DefaultMutate(uint8_t *Data, size_t Size,
                                         size_t MaxSize) {
  return MutateImpl(Data, Size, MaxSize, DefaultMutators);
}

// Mutates Data in place, returns new size.
size_t MutationDispatcher::MutateImpl(uint8_t *Data, size_t Size,
                                      size_t MaxSize,
                                      Vector<Mutator> &Mutators) {
  assert(MaxSize > 0);
  // Some mutations may fail (e.g. can't insert more bytes if Size == MaxSize),
  // in which case they will return 0.
  // Try several times before returning un-mutated data.
  for (int Iter = 0; Iter < 100; Iter++) {
    auto M = Mutators[Rand(Mutators.size())];
    size_t NewSize = (this->*(M.Fn))(Data, Size, MaxSize);
    if (NewSize && NewSize <= MaxSize) {
      if (Options.OnlyASCII)
        ToASCII(Data, NewSize);
      CurrentMutatorSequence.push_back(M);
      return NewSize;
    }
  }
  *Data = ' ';
  return 1; // Fallback, should not happen frequently.
}

// Mask represents the set of Data bytes that are worth mutating.
size_t MutationDispatcher::MutateWithMask(uint8_t *Data, size_t Size,
                                          size_t MaxSize,
                                          const Vector<uint8_t> &Mask) {
  size_t MaskedSize = std::min(Size, Mask.size());
  // * Copy the worthy bytes into a temporary array T
  // * Mutate T
  // * Copy T back.
  // This is totally unoptimized.
  auto &T = MutateWithMaskTemp;
  if (T.size() < Size)
    T.resize(Size);
  size_t OneBits = 0;
  for (size_t I = 0; I < MaskedSize; I++)
    if (Mask[I])
      T[OneBits++] = Data[I];

  if (!OneBits)
    return 0;
  assert(!T.empty());
  size_t NewSize = Mutate(T.data(), OneBits, OneBits);
  assert(NewSize <= OneBits);
  (void)NewSize;
  // Even if NewSize < OneBits we still use all OneBits bytes.
  for (size_t I = 0, J = 0; I < MaskedSize; I++)
    if (Mask[I])
      Data[I] = T[J++];
  return Size;
}

void MutationDispatcher::AddWordToManualDictionary(const Word &W) {
  ManualDictionary.push_back({W, std::numeric_limits<size_t>::max()});
}

} // namespace fuzzer
