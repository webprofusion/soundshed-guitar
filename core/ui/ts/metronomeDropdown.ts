/**
 * The metronome's select control.
 *
 * A native `<select>` is not enough here: the meter picker shows two columns
 * (quarter-note meters beside eighth-note ones) and every popup has to match
 * the app's own dark styling rather than the OS list the WebView draws. This
 * is a small, self-contained replacement — trigger button plus a panel of
 * options — used by all three metronome pickers.
 */

export interface DropdownOption {
  value: string;
  label: string;
  /** Options sharing a column key are rendered in the same column. */
  column?: string;
}

export interface DropdownConfig {
  trigger: HTMLButtonElement;
  panel: HTMLElement;
  onSelect: (value: string) => void;
}

/** Every open dropdown, so opening one closes the others. */
const openDropdowns = new Set<MetronomeDropdown>();

let documentListenersBound = false;

function bindDocumentListeners(): void {
  if (documentListenersBound) return;
  documentListenersBound = true;

  document.addEventListener("mousedown", (event) => {
    const target = event.target as Node | null;
    for (const dropdown of Array.from(openDropdowns)) {
      if (!dropdown.containsNode(target)) dropdown.close();
    }
  });

  document.addEventListener("keydown", (event) => {
    if (event.key !== "Escape" || !openDropdowns.size) return;
    for (const dropdown of Array.from(openDropdowns)) dropdown.close();
    event.stopPropagation();
  });
}

export class MetronomeDropdown {
  private readonly trigger: HTMLButtonElement;

  private readonly panel: HTMLElement;

  private readonly onSelect: (value: string) => void;

  private options: DropdownOption[] = [];

  private value = "";

  private isOpen = false;

  constructor(config: DropdownConfig) {
    this.trigger = config.trigger;
    this.panel = config.panel;
    this.onSelect = config.onSelect;

    this.trigger.setAttribute("aria-haspopup", "listbox");
    this.trigger.setAttribute("aria-expanded", "false");
    this.panel.setAttribute("role", "listbox");
    this.panel.hidden = true;

    this.trigger.addEventListener("click", (event) => {
      event.stopPropagation();
      if (this.trigger.disabled) return;
      if (this.isOpen) {
        this.close();
      } else {
        this.open();
      }
    });

    this.trigger.addEventListener("keydown", (event) => {
      if (event.key === "ArrowDown" && !this.isOpen) {
        event.preventDefault();
        this.open();
      }
    });

    bindDocumentListeners();
  }

  containsNode(node: Node | null): boolean {
    if (!node) return false;
    return this.trigger.contains(node) || this.panel.contains(node);
  }

  setOptions(options: DropdownOption[]): void {
    this.options = options;
    this.render();
    this.syncTriggerLabel();
  }

  setValue(value: string): void {
    this.value = value;
    this.render();
    this.syncTriggerLabel();
  }

  setDisabled(disabled: boolean): void {
    this.trigger.disabled = disabled;
    if (disabled) this.close();
  }

  open(): void {
    for (const other of Array.from(openDropdowns)) {
      if (other !== this) other.close();
    }

    this.isOpen = true;
    this.panel.hidden = false;
    this.trigger.setAttribute("aria-expanded", "true");
    this.trigger.classList.add("is-open");
    openDropdowns.add(this);

    this.panel.querySelector<HTMLElement>(".metro-select-option.is-selected")?.scrollIntoView({ block: "nearest" });
  }

  close(): void {
    if (!this.isOpen) return;
    this.isOpen = false;
    this.panel.hidden = true;
    this.trigger.setAttribute("aria-expanded", "false");
    this.trigger.classList.remove("is-open");
    openDropdowns.delete(this);
  }

  private syncTriggerLabel(): void {
    const label = this.trigger.querySelector<HTMLElement>(".metro-select-label") ?? this.trigger;
    const selected = this.options.find((option) => option.value === this.value);
    label.textContent = selected?.label ?? this.value ?? "";
  }

  private render(): void {
    this.panel.innerHTML = "";

    const columns = new Map<string, DropdownOption[]>();
    for (const option of this.options) {
      const key = option.column ?? "";
      const bucket = columns.get(key);
      if (bucket) {
        bucket.push(option);
      } else {
        columns.set(key, [option]);
      }
    }

    this.panel.classList.toggle("is-multi-column", columns.size > 1);

    for (const [, columnOptions] of columns) {
      const column = document.createElement("div");
      column.className = "metro-select-column";

      for (const option of columnOptions) {
        const item = document.createElement("button");
        item.type = "button";
        item.className = "metro-select-option";
        item.dataset.value = option.value;
        item.setAttribute("role", "option");

        const selected = option.value === this.value;
        item.classList.toggle("is-selected", selected);
        item.setAttribute("aria-selected", selected ? "true" : "false");

        const text = document.createElement("span");
        text.className = "metro-select-option-label";
        text.textContent = option.label;
        item.appendChild(text);

        if (selected) {
          const tick = document.createElement("span");
          tick.className = "metro-select-option-check";
          tick.setAttribute("aria-hidden", "true");
          tick.textContent = "✓";
          item.appendChild(tick);
        }

        item.addEventListener("click", (event) => {
          event.stopPropagation();
          this.close();
          if (option.value !== this.value) this.onSelect(option.value);
        });

        column.appendChild(item);
      }

      this.panel.appendChild(column);
    }
  }
}
