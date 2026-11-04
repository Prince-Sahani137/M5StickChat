import os
from SCons.Script import DefaultEnvironment

def configure(env):
    # Standard release optimization
    env.Append(CCFLAGS=["-Os"])
    env.Append(LINKFLAGS=["-Wl,--gc-sections"])

env = DefaultEnvironment()
configure(env)