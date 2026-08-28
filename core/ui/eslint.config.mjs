import js from "@eslint/js";
import tseslint from "typescript-eslint";

/**
 * Deliberately small ruleset. The job here is to catch what a large refactor
 * actually leaves behind — imports and locals orphaned by a move, promises
 * dropped on the floor, `import type` that should be marked as such — not to
 * relitigate a 67k-line codebase's style.
 *
 * Type-aware rules are on, so this needs the tsconfig and is slower than a
 * plain lint. That is the trade: `no-floating-promises` is one of the few
 * rules that finds real bugs in this code, and it requires type information.
 */
export default tseslint.config(
  {
    ignores: ["dist/**", "node_modules/**", "Testing/**", "scripts/**", "demo/**", "assets/**", "metronome/**"],
  },

  js.configs.recommended,
  ...tseslint.configs.recommendedTypeChecked,

  {
    languageOptions: {
      parserOptions: {
        // tsconfig.json deliberately covers only ts/ (it drives the emit).
        // Linting also needs tests/, so it gets its own no-emit project.
        project: ["./tsconfig.eslint.json"],
        tsconfigRootDir: import.meta.dirname,
      },
    },

    rules: {
      // ── What this refactor needs to catch ──────────────────────────────
      "@typescript-eslint/no-unused-vars": [
        "error",
        { argsIgnorePattern: "^_", varsIgnorePattern: "^_", caughtErrors: "none" },
      ],
      "@typescript-eslint/consistent-type-imports": [
        "error",
        { prefer: "type-imports", fixStyle: "separate-type-imports" },
      ],
      "@typescript-eslint/no-floating-promises": "error",

      // `arguments: false` allows `addEventListener("click", async () => ...)`,
      // which is how every async handler in this UI is written. The rule stays
      // on everywhere else; no-floating-promises above still covers the case
      // this is actually warning about — a promise nobody is watching.
      "@typescript-eslint/no-misused-promises": ["error", { checksVoidReturn: { arguments: false } }],

      // Off: nearly every hit is a function that is async to satisfy a shared
      // signature (unit builders, mocked fetch responses, Promise<void> panel
      // renderers). Removing `async` there would break the contract, so the
      // rule reports noise rather than defects in this codebase.
      "@typescript-eslint/require-await": "off",

      // Off: the base rule predates `import type` and flags the deliberate
      // split between a module's type imports and its value imports. That
      // split is worth keeping — only value imports exist at runtime, so it is
      // what makes the real dependency graph readable at the top of a file.
      "no-duplicate-imports": "off",

      // ── Turned off: pre-existing patterns, not regressions ─────────────
      // The DOM/bridge boundary is genuinely untyped; 31 `any` uses are
      // deliberate and reviewed. Flagging them would bury the signal above.
      "@typescript-eslint/no-explicit-any": "off",
      "@typescript-eslint/no-unsafe-assignment": "off",
      "@typescript-eslint/no-unsafe-member-access": "off",
      "@typescript-eslint/no-unsafe-call": "off",
      "@typescript-eslint/no-unsafe-argument": "off",
      "@typescript-eslint/no-unsafe-return": "off",
      // Template literals interpolate numbers constantly in markup builders.
      "@typescript-eslint/restrict-template-expressions": "off",
      "@typescript-eslint/no-empty-function": "off",
      "@typescript-eslint/no-non-null-assertion": "off",
      // Disabled after it produced ~200 false positives here: it reports
      // `querySelector(...) as HTMLElement | null` as redundant, but the emit
      // compiler types querySelector as `Element | null`, so removing the
      // assertion breaks every following `.style` / `.click()` / `.dataset`.
      // Its autofix is therefore not safe against this project's types.
      "@typescript-eslint/no-unnecessary-type-assertion": "off",
      // `try { ... } catch {}` is the house idiom for optional DOM/host calls
      // that are allowed to fail silently. It is intentional, not an oversight.
      "no-empty": ["error", { allowEmptyCatch: true }],
    },
  },

  {
    files: ["tests/**/*.ts"],
    rules: {
      "@typescript-eslint/no-unused-vars": "off",
      // Test doubles stringify URL objects and stand in for fetch responses;
      // both are fine in a test and neither is worth reshaping to satisfy a rule.
      "@typescript-eslint/no-base-to-string": "off",
    },
  }
);
