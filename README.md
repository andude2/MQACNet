# MQACNet

MQACNet is an actor-based MacroQuest command bus. It sends commands and small control messages through MacroQuest's internal post office / actor system instead of an external networking library or EQBC server.

Load it on every client that should send or receive commands:

```txt
/plugin MQACNet
```

## Command Execution

All command execution commands send the text after the MQACNet command to matching clients. The command giver is excluded unless the command name ends with `a` for "all including me".

```txt
/ac /say hi
/aca /say hi
/acaa /say hi
```

Target one character:

```txt
/actell Cleric01 /cast "Complete Heal" -targetid|${Me.ID}
/act Cleric01 /say targeted command
/acexecute Cleric01 /foreground
/acex Cleric01 /foreground
```

Group:

```txt
/acg /nav id ${Me.ID}
/acga /say group including me
/acgz /nav id ${Me.ID}
/acgza /say group in zone including me
```

Raid:

```txt
/acr /nav id ${Me.ID}
/acra /say raid including me
/acrz /nav id ${Me.ID}
/acrza /say raid in zone including me
```

Zone:

```txt
/acze /say everyone in zone except me
/acza /say everyone in zone including me
/aczea /say everyone in zone including me
```

Selector execution:

```txt
/acge healer /target id ${Me.ID}
/acgae caster /say casters including me
/acge raid /nav id ${Me.ID}
/acge war /say warriors only
```

Supported selectors:

```txt
all, zone, group, raid, tank, priest, healer, melee, caster
class names and short names, such as Warrior, WAR, war, Cleric, CLR, clr
```

## Delay And Stagger

Execution broadcasts support optional `-delay <ms>` and `-stagger <ms>` before the command text.

```txt
/acr -delay 1000 /say delayed raid command
/acr -stagger 250 /nav id ${Me.ID}
/acaa -delay 500 -stagger 100 /say staggered all clients
```

`-delay` waits before executing on every receiver. `-stagger` adds an additional per-recipient offset based on recipient order when a roster is available.

## Status And Discovery

MQACNet sends a lightweight presence heartbeat every five seconds. Use status commands to list known peers and request live replies:

```txt
/acstatus
/acwho
/acping
```

Status output includes character, zone, class, and last-seen time for peers that have been heard from.

## Messages

Send non-executing AC chat messages:

```txt
/acmsg raid Burn in 10
/acmsg group Moving now
/acmsg healer Rebuff after event
/acmsgto Cleric01 Rez incoming
```

Messages print in the receiver's MQ chat as `[AC] Sender: message`.

## Queries

Ask peers to evaluate a MacroQuest expression and reply with the result:

```txt
/acquery group ${Me.PctHPs}
/acquery raid ${Me.CurrentMana}
/acquery healer ${Me.SpellReady[Complete Heal]}
/acqueryto Cleric01 ${Me.Gem[Complete Heal]}
```

Replies print in MQ chat with the query id and sender.

## Foreground Helpers

Bring matching clients to the foreground using MacroQuest's built-in `/foreground` command:

```txt
/acfg group
/acfg raid
/acfg healer
/acfgto Warrior01
```

## Notes

MQACNet only uses MacroQuest internal actor routing. There is no external server process and no external networking dependency.

All receiving clients must have MQACNet loaded. If a DLL is already loaded, unload and reload the plugin after replacing it:

```txt
/plugin MQACNet unload
/plugin MQACNet
```

