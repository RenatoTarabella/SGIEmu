# Emulatore SGI — analisi preliminare

Note raccolte il 29/09/2026, prima di iniziare il progetto. Le informazioni sullo stato
di MAME, GXemul e dei driver Linux arrivano a metà 2026: vanno ricontrollate all'avvio.

## Obiettivo

Un emulatore di workstation SGI **veloce**, con IRIX usabile sul desktop. Non si cerca la
fedeltà al ciclo, che invece serviva sull'Archimedes per i giochi.

Il punto di partenza è che l'emulatore esistente (MAME) è lentissimo. Provato con l'R4400,
il suo JIT non funziona e si ricade sull'interprete.

## Quale macchina

| Macchina | Scheda | Documentazione | Note |
|---|---|---|---|
| **Indy** | IP24 | la migliore | grafica Newport (XL): rasterizzatore 2D con primitive 3D minime |
| **Indigo2** | IP22 | la migliore | stessi ASIC dell'Indy; con Newport è la stessa strada, con IMPACT è ricerca |
| Indigo (R4000/R4400) | IP20 | discreta | MAME ha un driver; chipset più vecchio dell'IP22 |
| O2 | IP32 | scarsa | memoria unificata CRIME/MACE, più difficile |
| Octane | IP30 | parziale | Heart/Xbow/Bridge, supportata da Linux (Skowronek, Kinard) |
| Onyx / Origin | IP19/IP25/IP27 | quasi nulla | fuori portata |

**Consiglio: Indy (IP24), poi Indigo2 (IP22).**
- È la piattaforma meglio coperta da Linux, NetBSD e MAME.
- Ha un solo ASIC grafico relativamente semplice (Newport / REX3).
- Ha IRIX 5.3 e 6.5 come obiettivo concreto.

L'Indigo IP20 con R4400 è un'alternativa possibile, ma condivide poco con l'IP22/24.
Conviene arrivarci dopo, riusando CPU e parte delle periferiche.

## Documentazione disponibile

**Processore: documentato bene.**
- MIPS R4000 Microprocessor User's Manual (Joe Heinrich): istruzioni, TLB, cache, eccezioni.
  Basta per l'emulatore.
- Manuali di R4400, R4600/R5000 e R10000: bitsavers.org, cartella `pdf/mips`.

**ASIC SGI: mai pubblicati, si ricostruiscono da:**
- **Linux**: `arch/mips/sgi-ip22` (Indy/Indigo2), `sgi-ip32` (O2), `sgi-ip27` (Origin),
  `sgi-ip30` (Octane). Poi i driver: Newport (`newport_con`, `newport.h`), HPC3,
  WD33C93 (SCSI), Seeq 8003 (ethernet), Zilog 85C30 (seriali), PS/2, DS1286 (RTC).
- **NetBSD / OpenBSD sgimips**: spesso commentati meglio di Linux.
- **MAME**: il driver Indy/Indigo2 (MC, HPC3, IOC2, INT2/3, Newport/REX3, VINO...) è la
  "documentazione eseguibile" più completa. Licenza BSD/GPL: si studia per capire il
  comportamento dei chip, **non si copia il codice** (porterebbe con sé la licenza).
- **GXemul**: supporto parziale per diverse macchine SGI, codice più leggibile di MAME.
- **bitsavers.org, cartella `pdf/sgi`**: manuali hardware, schemi, qualche specifica interna.
- **Comunità**: forums.irixnet.org, e chi lavora al driver SGI di MAME.

**Dove manca la documentazione:** la grafica di fascia alta. IMPACT/MGRAS è stata ricostruita
in parte per Linux; VPro/Odyssey poco; RealityEngine e InfiniteReality quasi nulla.

**Materiale da procurarsi** (non ridistribuibile, come le ROM di RISC OS):
- la PROM di un Indy (IP24) o di un Indigo2 (IP22);
- i CD di installazione di IRIX 5.3 e/o 6.5.

## Perché MAME è lento

- Lo scheduler fa avanzare tutti i dispositivi a piccole fette di tempo sincronizzate:
  ottimo per la precisione, pesante per una workstation.
- La grafica Newport è emulata operazione per operazione, pixel per pixel, come il chip.
- Tutto passa per un framework generico, pensato per migliaia di macchine diverse.

## Dove si può fare meglio

1. **CPU con JIT verso x86-64.** MIPS è quasi l'architettura ideale da ricompilare:
   istruzioni a lunghezza fissa, niente flag, pochi casi strani.
2. **MMU veloce.** Una tabella che traduce direttamente indirizzo virtuale → puntatore host
   (come la softmmu di QEMU), invalidata quando IRIX tocca il TLB. È il punto dove si
   guadagna o si perde di più.
3. **Niente cicli.** Un contatore di istruzioni per Count/Compare e gli interrupt. Il ciclo
   di idle di IRIX si salta quando non ha niente da fare.
4. **Grafica ad alto livello.** I comandi REX3 (rettangoli, linee, copie di blocchi)
   diventano cicli ottimizzati nel framebuffer host, o sulla GPU, senza simulare la
   pipeline del chip.
5. **Periferiche "a transazione".** SCSI ed ethernet emulati per comando, senza i tempi del bus.

## Perché un JIT per R4400 si rompe sotto IRIX

1. **TLB che cambia di continuo.** A ogni cambio di processo cambia l'ASID e gli stessi
   indirizzi virtuali puntano ad altro. Un JIT indicizzato per indirizzo virtuale o esegue
   codice vecchio o butta via tutto di continuo.
2. **Eccezioni precise.** Qualunque load o store può dare TLB miss, e IRIX gestisce la
   paginazione via software. Serve EPC esatto e uno stato dei registri coerente.
3. **Delay slot.** Se l'eccezione cade nello slot dopo un salto, serve il bit BD in Cause ed
   EPC sul salto. È un classico punto dove i JIT sbagliano.
4. **Codice che cambia.** Il kernel carica programmi, il dynamic linker riloca, le pagine
   vengono riusate. I blocchi tradotti vanno invalidati quando la loro pagina viene scritta.
5. **Cache e istruzione CACHE.** La PROM testa le cache e IRIX le gestisce a mano: il
   comportamento dei tag va simulato almeno in parte. Ci sono poi gli alias KSEG0/KSEG1
   (la stessa memoria, cached e uncached).
6. **Timer e interrupt.** Count/Compare devono avanzare in modo credibile e gli interrupt
   vanno controllati ai confini dei blocchi, altrimenti lo scheduler di IRIX impazzisce.
7. **64 bit e FPU.** Estensione del segno nei registri a 64 bit, LL/SC, trap FPU per i
   denormali (IRIX li emula nel kernel), eccezioni FPU precise.

## Impostazione proposta

- **Blocchi indicizzati per indirizzo fisico.** All'ingresso di un blocco si traduce il PC;
  dentro, gli accessi a memoria passano dalla tabella veloce virtuale → host. Un cambio di
  TLB invalida solo quella tabella, non il codice tradotto.
- **Ogni load/store salva il PC prima dell'accesso.** È una scrittura di costante, quasi
  gratis, e dà eccezioni precise sempre, delay slot compresi.
- **Pagine con codice tradotto marcate.** Una scrittura su una di quelle pagine invalida i
  blocchi relativi.
- **Due livelli.**
  - Prima un interprete veloce, con istruzioni pre-decodificate per pagina fisica: già
    diverse volte più rapido di un interprete ingenuo, e quasi altrettanto robusto.
  - Poi il JIT sopra, che per i casi strani (CACHE, istruzioni rare, blocchi problematici)
    ricade sull'interprete.
- **Verifica differenziale**, come con Unicorn per l'ARM di ArchieEmu. Una modalità lockstep
  esegue lo stesso codice con interprete e JIT e si ferma alla prima divergenza dei
  registri. È lo strumento che rende un JIT affidabile invece che "quasi funzionante".

In pratica la velocità non viene tanto dal JIT in sé, ma dall'averlo costruito attorno a come
IRIX usa MMU ed eccezioni. Già l'interprete pre-decodificato con MMU veloce dovrebbe essere
parecchio più rapido di MAME.

## Architettura (dall'esperienza di ArchieEmu)

Stessa struttura modulare di `..\ARM`:
- CPU come modulo (R4400: interprete, poi JIT);
- bus / MMU;
- un modulo per ogni ASIC: MC (memoria), HPC3 (DMA, periferiche), IOC2/INT2 (interrupt,
  timer 8254), Newport/REX3 (grafica), WD33C93 (SCSI), Seeq (ethernet), Z85C30 (seriali),
  DS1286 (RTC e NVRAM);
- frontend Win32;
- strumenti da riga di comando per seguire il boot, come `archie_boot`: log della
  console seriale, screenshot PNG, tracce.

Toolchain già pronta sul PC: MSVC 2022 + CMake; Python con Unicorn, che supporta MIPS e può
fare da oracolo per l'ALU come fatto per l'ARM.

## Tappe

1. **CPU R4400**: interprete a 32 e 64 bit, CP0, TLB, eccezioni, FPU. Test contro Unicorn.
2. **Prompt della PROM**: CPU + MMU + MC + HPC3 + seriale. La PROM parla sulla seriale prima
   ancora della grafica, quindi la console seriale è il primo "schermo".
3. **Newport minima**: la PROM grafica e il suo menu.
4. **SCSI e disco**: boot di sash / fx, installazione di IRIX da immagine CD.
5. **IRIX multiutente, poi il desktop** (4Dwm, Indigo Magic).
6. **Velocità**: interprete pre-decodificato, poi JIT con lockstep.
7. Più avanti: Indigo2, ethernet, audio (HAL2), forse IMPACT.

È un progetto di un altro ordine di grandezza rispetto ad ArchieEmu: mesi, non giorni.
