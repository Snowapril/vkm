#ifndef TEST_HALF_FLOAT_SHARED_HPP
#define TEST_HALF_FLOAT_SHARED_HPP

#include <cmath>
#include <cstdint>

/*
* IEEE-754 binary16 conversion for tests that author or read back RGBA16F textures. Small enough
* to keep here rather than pulling in a half-float dependency for a handful of assertions.
*/

namespace vkmtest
{
    inline float decodeHalf(uint16_t bits)
    {
        const uint32_t sign = static_cast<uint32_t>(bits >> 15) & 0x1u;
        const uint32_t exponent = static_cast<uint32_t>(bits >> 10) & 0x1Fu;
        const uint32_t mantissa = static_cast<uint32_t>(bits) & 0x3FFu;

        float value = 0.0f;
        if (exponent == 0)
        {
            value = std::ldexp(static_cast<float>(mantissa), -24); // subnormal (and zero)
        }
        else if (exponent == 31)
        {
            value = mantissa == 0 ? INFINITY : NAN;
        }
        else
        {
            value = std::ldexp(static_cast<float>(mantissa + 1024u), static_cast<int>(exponent) - 25);
        }
        return sign != 0 ? -value : value;
    }

    // Round-to-nearest-even is not needed here: test values are chosen to be representable, and
    // truncation keeps this short and obviously correct.
    inline uint16_t encodeHalf(float value)
    {
        if (value == 0.0f)
        {
            return 0;
        }
        const uint16_t sign = value < 0.0f ? 0x8000u : 0x0000u;
        float magnitude = std::fabs(value);

        int exponent = 0;
        float mantissa = std::frexp(magnitude, &exponent); // magnitude = mantissa * 2^exponent, mantissa in [0.5, 1)
        // Re-normalize to [1, 2) so the implicit leading bit matches binary16's encoding.
        mantissa *= 2.0f;
        exponent -= 1;

        const int biasedExponent = exponent + 15;
        if (biasedExponent >= 31)
        {
            return static_cast<uint16_t>(sign | 0x7C00u); // infinity
        }
        if (biasedExponent <= 0)
        {
            const float subnormal = magnitude / std::ldexp(1.0f, -24);
            return static_cast<uint16_t>(sign | static_cast<uint16_t>(subnormal));
        }
        const uint16_t mantissaBits = static_cast<uint16_t>((mantissa - 1.0f) * 1024.0f);
        return static_cast<uint16_t>(sign | static_cast<uint16_t>(biasedExponent << 10) | mantissaBits);
    }

    inline float readHalfComponent(const uint8_t* texel, size_t component)
    {
        const uint16_t bits = static_cast<uint16_t>(texel[component * 2]) |
                              static_cast<uint16_t>(static_cast<uint16_t>(texel[component * 2 + 1]) << 8);
        return decodeHalf(bits);
    }
}

#endif // TEST_HALF_FLOAT_SHARED_HPP
