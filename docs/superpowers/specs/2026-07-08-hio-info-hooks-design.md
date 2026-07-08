# Design: Aplikační hooky pro `hio info` (shell + ATCI)

Datum: 2026-07-08
Stav: návrh ke schválení

## Cíl

Umožnit aplikaci rozšířit výstup modulu `hio_info` o vlastní klíče, a to:

- v shell příkazu `info show` (na konci výpisu),
- v AT příkazu `AT$INFO?` (na konci výpisu),
- v lookupu `AT$INFO="klíč"`,
- v enumeraci `AT$INFO=?`.

Vestavěné položky SDK zůstávají beze změny (pevné pořadí, žádná migrace
`info_items[]` ani shell subpříkazů). Aplikační klíče se řadí podle pořadí
registrace — proto runtime registrace, ne linkerové sekce (ty řadí abecedně
podle jména symbolu).

## API (`include/hio/hio_info.h`)

```c
#include <zephyr/sys/slist.h>   /* jediný nový include (sys_snode_t) */

struct shell;      /* forward deklarace — bez závislosti na shell/ATCI hlavičkách */
struct hio_atci;

struct hio_info_hook {
	sys_snode_t node;   /* interní, aplikace nevyplňuje */

	/* Klíč pro AT$INFO="name" a AT$INFO=?; NULL = hook se v lookupu
	 * a enumeraci neobjeví (jen v `info show` / AT$INFO?). */
	const char *name;

	/* Popisek pro AT$INFO=?; smí být NULL, pak se tiskne "". */
	const char *label;

	/* Vytiskne řádek/řádky do shellu (shell_print). NULL = přeskočit. */
	void (*shell)(const struct shell *shell);

	/* Vytiskne $INFO řádek/řádky (hio_atci_printfln). NULL = přeskočit.
	 * Návratová hodnota se propaguje jako výsledek u AT$INFO="name";
	 * při výpisu AT$INFO? se ignoruje. */
	int (*atci)(const struct hio_atci *atci);
};

int hio_info_hook_register(struct hio_info_hook *hook);
```

- Struct vlastní aplikace (statická alokace); SDK nealokuje nic.
- `hio_info_hook_register()` dělá append do `sys_slist_t` → pořadí výpisu
  odpovídá pořadí registrace. Vrací 0; duplicitní jména se nekontrolují.
- Bez zamykání: registrace se předpokládá při initu aplikace, před aktivací
  shellu/ATCI. Zdokumentováno v hlavičce.

## Změny v SDK

### `subsys/hio_info/hio_info.c`

- `sys_slist_t` se seznamem hooků + `hio_info_hook_register()`.
- Interní iterátor pro shell/ATCI (např. `hio_info_hook_list_get()` vracející
  slist, nebo foreach helper) — detail nechán na implementaci.

### `subsys/hio_info/hio_info_shell.c`

- Na konec `cmd_show()` doplnit průchod seznamem: pro každý hook s ne-NULL
  `shell` callbackem jej zavolat. Formát řádku je plně v režii aplikace
  (konvence: `label: hodnota`, stejně jako vestavěné položky).

### `subsys/hio_info/hio_info_atci.c`

- `at_info_read()` (`AT$INFO?`): po smyčce přes `info_items[]` zavolat `atci`
  callbacky všech hooků (návratové hodnoty se ignorují, pokračuje se dál).
  Konvence výstupu: `$INFO: "name","hodnota"`.
- `at_info_set()` (`AT$INFO="klíč"`): pokud klíč nesedí na žádnou vestavěnou
  položku, projít hooky a porovnat s `hook->name`; při shodě zavolat `atci`
  callback a vrátit jeho návratovou hodnotu. Jinak stávající chování
  („Item not found“).
- `at_info_test()` (`AT$INFO=?`): za vestavěnými položkami vypsat hooky
  s ne-NULL `name`: `$INFO: "name","string","label"` (typ je vždy `string`,
  `label==NULL` → prázdný řetězec).

## Chování a okrajové případy

- Hook s oběma callbacky NULL je legální a nic nedělá.
- Hook může tisknout i více řádků — SDK to nijak neomezuje. U víceřádkových
  hooků je na aplikaci, aby v ATCI dodržela `$INFO:` prefix.
- `name` bez `atci` callbacku: v `AT$INFO=?` se klíč vypíše, ale
  `AT$INFO="name"` vrátí úspěch bez výstupu — nedoporučeno, zdokumentovat.
- Vestavěné AT příkazy (`ATI`, `+CGMI`, …) a individuální shell subpříkazy
  (`info vendor-name`, …) se nemění.

## Testování

- Ověření v aplikaci (ksb-guard-clm): registrovat 2 hooky při initu, ověřit:
  - `info show` — klíče na konci, ve správném pořadí registrace,
  - `AT$INFO?` — totéž,
  - `AT$INFO="klíč"` — vrátí jen daný řádek, neexistující klíč dál vrací
    „Item not found“,
  - `AT$INFO=?` — aplikační klíče na konci seznamu,
  - hook s `name == NULL` se objeví jen ve výpisech, ne v enumeraci/lookupu.
- Build bez registrovaných hooků musí projít beze změny chování (prázdný
  seznam = žádný výstup navíc).
