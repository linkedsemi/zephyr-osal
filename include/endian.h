#ifndef ENDIAN_H_
#define ENDIAN_H_
#include <stdint.h>
#include <byteswap.h>

// uint16_t htobe16(uint16_t value);
// inline uint16_t htole16(uint16_t value)
// {
//     return value;
// }

// uint16_t be16toh(uint16_t value);
// uint16_t le16toh(uint16_t value);

// uint32_t htobe32(uint32_t value);
// uint32_t htole32(uint32_t value);
// uint32_t be32toh(uint32_t value);
// uint32_t le32toh(uint32_t value);

// uint64_t htobe64(uint64_t value);
// uint64_t htole64(uint64_t value);
// uint64_t be64toh(uint64_t value);
// uint64_t le64toh(uint64_t value);
#ifdef __CHECKER__
#define __sd_bitwise __attribute__((__bitwise__))
#define __sd_force __attribute__((__force__))
#else
#define __sd_bitwise
#define __sd_force
#endif

// typedef uint16_t __sd_bitwise le16_t;
// typedef uint16_t __sd_bitwise be16_t;
// typedef uint32_t __sd_bitwise le32_t;
// typedef uint32_t __sd_bitwise be32_t;
// typedef uint64_t __sd_bitwise le64_t;
// typedef uint64_t __sd_bitwise be64_t;

#undef htobe16
#undef htole16
#undef be16toh
#undef le16toh
#undef htobe32
#undef htole32
#undef be32toh
#undef le32toh
#undef htobe64
#undef htole64
#undef be64toh
#undef le64toh

#if __BYTE_ORDER == __LITTLE_ENDIAN
#define bswap_16_on_le(x) bswap_16(x)
#define bswap_32_on_le(x) bswap_32(x)
#define bswap_64_on_le(x) bswap_64(x)
#define bswap_16_on_be(x) (x)
#define bswap_32_on_be(x) (x)
#define bswap_64_on_be(x) (x)
#elif __BYTE_ORDER == __BIG_ENDIAN
#define bswap_16_on_le(x) (x)
#define bswap_32_on_le(x) (x)
#define bswap_64_on_le(x) (x)
#define bswap_16_on_be(x) bswap_16(x)
#define bswap_32_on_be(x) bswap_32(x)
#define bswap_64_on_be(x) bswap_64(x)
#endif

inline uint16_t htole16(uint16_t value) { return (uint16_t __sd_force) bswap_16_on_be(value); }
inline uint32_t htole32(uint32_t value) { return (uint32_t __sd_force) bswap_32_on_be(value); }
inline uint64_t htole64(uint64_t value) { return (uint64_t __sd_force) bswap_64_on_be(value); }

inline uint16_t htobe16(uint16_t value) { return (uint16_t __sd_force) bswap_16_on_le(value); }
inline uint32_t htobe32(uint32_t value) { return (uint32_t __sd_force) bswap_32_on_le(value); }
inline uint64_t htobe64(uint64_t value) { return (uint64_t __sd_force) bswap_64_on_le(value); }

inline uint16_t le16toh(uint16_t value) { return bswap_16_on_be((uint16_t __sd_force)value); }
inline uint32_t le32toh(uint32_t value) { return bswap_32_on_be((uint32_t __sd_force)value); }
inline uint64_t le64toh(uint64_t value) { return bswap_64_on_be((uint64_t __sd_force)value); }

inline uint16_t be16toh(uint16_t value) { return bswap_16_on_le((uint16_t __sd_force)value); }
inline uint32_t be32toh(uint32_t value) { return bswap_32_on_le((uint32_t __sd_force)value); }
inline uint64_t be64toh(uint64_t value) { return bswap_64_on_le((uint64_t __sd_force)value); }

#undef __sd_bitwise
#undef __sd_force


#endif

