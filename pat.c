/*
 * pat.c: vtsatip - PAT parser and PMT PID cache
 */

#include "pat.h"

cVtPatParser::cVtPatParser(void)
: filterM(new cVtSecFilter(0, 0x00, 0xFF))
{
}

cVtPatParser::~cVtPatParser()
{
  delete filterM;
}

void cVtPatParser::Reset(void)
{
  cMutexLock lock(&mutexM);
  filterM->Reset();
}

uint32_t cVtPatParser::Crc32(const uchar *dataP, int lenP)
{
  uint32_t crc = 0xFFFFFFFF;
  for (int i = 0; i < lenP; ++i) {
      crc ^= (uint32_t)dataP[i] << 24;
      for (int b = 0; b < 8; ++b)
          crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : (crc << 1);
      }
  return crc;
}

bool cVtPatParser::ParseSection(const uchar *dataP, int lenP, std::map<int, int> &programsP)
{
  if (lenP < 12 || dataP[0] != 0x00 || !(dataP[1] & 0x80))
     return false;
  int total = 3 + (((dataP[1] & 0x0F) << 8) | dataP[2]);
  if (total > lenP || total < 12)
     return false;
  if (Crc32(dataP, total) != 0) // a section including its CRC yields 0
     return false;
  programsP.clear();
  for (int i = 8; i + 4 <= total - 4; i += 4) {
      int sid = (dataP[i] << 8) | dataP[i + 1];
      int pid = ((dataP[i + 2] & 0x1F) << 8) | dataP[i + 3];
      if (sid != 0) // program 0 points to the NIT
         programsP[sid] = pid;
      }
  return true;
}

bool cVtPatParser::Process(const uchar *tsP, std::map<int, int> &programsP)
{
  if (VtTsPid(tsP) != 0)
     return false;
  cMutexLock lock(&mutexM);
  filterM->Process(tsP);
  std::string section;
  bool found = false;
  while (filterM->Pop(section))
        if (ParseSection((const uchar *)section.data(), (int)section.size(), programsP))
           found = true;
  return found;
}

cMutex cVtPmtCache::mutexS;
std::map<std::tuple<int, int, int>, int> cVtPmtCache::cacheS;

void cVtPmtCache::Set(int sourceP, int transponderP, int sidP, int pmtPidP)
{
  cMutexLock lock(&mutexS);
  cacheS[std::make_tuple(sourceP, transponderP, sidP)] = pmtPidP;
}

int cVtPmtCache::Get(int sourceP, int transponderP, int sidP)
{
  cMutexLock lock(&mutexS);
  std::map<std::tuple<int, int, int>, int>::const_iterator it = cacheS.find(std::make_tuple(sourceP, transponderP, sidP));
  return it == cacheS.end() ? 0 : it->second;
}
