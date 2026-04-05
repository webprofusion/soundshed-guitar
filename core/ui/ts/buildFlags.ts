export interface BuildFlags {
  jamEnabled: boolean;
}

const DEFAULT_BUILD_FLAGS: BuildFlags = {
  jamEnabled: true,
};

function readBuildFlags(): BuildFlags {
  const raw = window.SOUNDSHED_BUILD_FLAGS;
  if (!raw || typeof raw !== "object") {
    return DEFAULT_BUILD_FLAGS;
  }

  return {
    jamEnabled: raw.jamEnabled !== false,
  };
}

export const buildFlags: BuildFlags = Object.freeze(readBuildFlags());

export function isJamEnabled(): boolean {
  return buildFlags.jamEnabled;
}

export function applyBuildFlags(): void {
  if (buildFlags.jamEnabled) {
    return;
  }

  document.querySelectorAll<HTMLElement>('[data-panel="jam"]').forEach((element) => {
    element.remove();
  });

  const jamPanel = document.getElementById("panel-jam");
  if (jamPanel instanceof HTMLElement) {
    jamPanel.remove();
  }

  const jamDock = document.getElementById("jam-player-dock");
  if (jamDock instanceof HTMLElement) {
    jamDock.remove();
  }

  const jamPlayerRoot = document.getElementById("jam-floating-player-root");
  if (jamPlayerRoot instanceof HTMLElement) {
    jamPlayerRoot.remove();
  }
}