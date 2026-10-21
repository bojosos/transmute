#pragma once
struct AlchemyPayload { int value; };
struct AlchemyBase { virtual ~AlchemyBase() = default; };
struct AlchemyDerived : AlchemyBase { int value{42}; };
extern "C" void alchemy_throw(int* destructors);
extern "C" AlchemyBase* alchemy_object();
