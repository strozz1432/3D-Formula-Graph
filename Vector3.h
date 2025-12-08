#pragma once

#include <iostream>

struct Vector3
{
    float x, y, z;
    Vector3() : x(0), y(0), z(0) {}
    Vector3(float x, float y, float z) : x(x), y(y), z(z) {}

    Vector3 operator+(const Vector3& other) const { return Vector3(x + other.x, y + other.y, z + other.z); }
    Vector3 operator-(const Vector3& other) const { return Vector3(x - other.x, y - other.y, z - other.z); }
    Vector3 operator*(float s) const { return Vector3(x * s, y * s, z * s); }
    Vector3 Dot(const Vector3& other) const { return Vector3(x * other.x, y * other.y, z * other.z); }
    Vector3 Cross(const Vector3& other) const {
        return Vector3(
            y * other.z - z * other.y,
            z * other.x - x * other.z,
            x * other.y - y * other.x
        );
    }
};

inline std::istream& operator>>(std::istream& in, Vector3& v) { in >> v.x >> v.y >> v.z; return in; }
inline std::ostream& operator<<(std::ostream& os, const Vector3& v) { os << "(" << v.x << ", " << v.y << ", " << v.z << ")"; return os; }
