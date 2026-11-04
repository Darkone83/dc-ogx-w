#ifndef OGXM_REMOTE_PROFILE_H
#define OGXM_REMOTE_PROFILE_H

#include "RemoteControl/Json.h"
#include "UserSettings/UserProfile.h"

namespace RemoteControl {

bool patch_profile(const Json& json, const UserProfile& current,
                   UserProfile& candidate, const char*& error);
bool validate_profile(const UserProfile& profile, const char*& error);
void write_profile(JsonWriter& writer, const UserProfile& profile);

} // namespace RemoteControl
#endif
