import os
import ssl

import superinvoke
from superinvoke.constants import Platforms

from . import tasks
from .envs import Envs
from .tools import Tools


# Temporal fix very annoying error: certificate verify failed: unable to get local issuer certificate
ssl._create_default_https_context = ssl._create_unverified_context

root = superinvoke.init(tools=Tools, envs=Envs)

root.configure(
    {
        "run": {
            "env": {"BOW_ROOT": os.getcwd()},
            "pty": (Platforms.CURRENT != Platforms.WINDOWS and Envs.Current != Envs.Ci),
        },
    }
)

root.add_task(tasks.lint)
root.add_task(tasks.plot)
root.add_task(tasks.erase)
