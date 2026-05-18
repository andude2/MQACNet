// MQACNet.cpp : Actor-based in-process MacroQuest command broadcasting.

#include <mq/Plugin.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

PreSetup("MQACNet");
PLUGIN_VERSION(0.1);

namespace {

constexpr std::string_view MailboxName = "acnet";
constexpr std::string_view PayloadHeader = "MQACNET2";

enum class MessageKind
{
	Execute,
	Chat,
	StatusRequest,
	StatusReply,
	QueryRequest,
	QueryReply,
	Foreground,
	Presence,
};

enum class Audience
{
	All,
	Group,
	Raid,
	Target,
	Selector,
};

struct NetCommand
{
	MessageKind Kind = MessageKind::Execute;
	Audience AudienceType = Audience::All;
	bool IncludeSelf = false;
	bool ZoneOnly = false;
	int DelayMs = 0;
	int StaggerMs = 0;
	uint32_t Id = 0;
	std::string Sender;
	std::string Zone;
	std::string Selector;
	std::string Command;
};

struct PeerInfo
{
	std::string Character;
	std::string Zone;
	std::string ClassName;
	uint64_t LastSeen = 0;
};

postoffice::DropboxAPI s_dropbox;
std::unordered_map<std::string, PeerInfo> s_peers;
uint64_t s_nextPresence = 0;
uint32_t s_nextMessageId = 1;

std::string Trimmed(const char* line)
{
	return std::string(trim(line ? std::string_view(line) : std::string_view()));
}

std::string CurrentCharacter()
{
	if (pLocalPC)
		return pLocalPC->Name;

	if (pLocalPlayer)
		return pLocalPlayer->Name;

	return {};
}

std::string CurrentZone()
{
	if (pZoneInfo)
		return pZoneInfo->ShortName;

	return {};
}

bool IsReady()
{
	return GetGameState() == GAMESTATE_INGAME && !CurrentCharacter().empty();
}

uint64_t NowMs()
{
	return GetTickCount64();
}

std::string CurrentClass()
{
	if (!pLocalPlayer)
		return {};

	const int classId = pLocalPlayer->GetClass();
	if (classId <= 0 || classId > TotalPlayerClasses)
		return {};

	return ClassInfo[classId].UCShortName;
}

void UpdatePeer(std::string_view character, std::string_view zone, std::string_view className)
{
	const std::string currentCharacter = CurrentCharacter();
	if (character.empty() || (!currentCharacter.empty() && ci_equals(currentCharacter, character)))
		return;

	PeerInfo& peer = s_peers[std::string(character)];
	peer.Character = character;
	peer.Zone = zone;
	peer.ClassName = className;
	peer.LastSeen = NowMs();
}

bool IsSender(std::string_view character)
{
	const std::string currentCharacter = CurrentCharacter();
	return !currentCharacter.empty() && ci_equals(currentCharacter, character);
}

bool IsCharacterInGroup(std::string_view character)
{
	if (IsSender(character))
		return true;

	if (!pLocalPC || !pLocalPC->Group)
		return false;

	for (uint32_t i = 0; i < pLocalPC->Group->GetMaxGroupSize(); ++i)
	{
		CGroupMember* member = pLocalPC->Group->GetGroupMember(i);
		if (member && member->GetName() && ci_equals(member->GetName(), character))
			return true;
	}

	return false;
}

bool IsCharacterInRaid(std::string_view character)
{
	if (IsSender(character))
		return true;

	if (!pRaid)
		return false;

	for (int i = 0; i < MAX_RAID_SIZE; ++i)
	{
		if (pRaid->RaidMemberUsed[i] && ci_equals(pRaid->RaidMember[i].Name, character))
			return true;
	}

	return false;
}

void AddRecipient(std::vector<std::string>& recipients, const char* name)
{
	if (!name || name[0] == 0)
		return;

	if (std::find_if(recipients.begin(), recipients.end(),
		[name](const std::string& recipient) { return ci_equals(recipient, name); }) == recipients.end())
	{
		recipients.emplace_back(name);
	}
}

std::string JoinRecipients(const std::vector<std::string>& recipients)
{
	std::string result;
	for (const std::string& recipient : recipients)
	{
		if (!result.empty())
			result += '|';

		result += recipient;
	}

	return result;
}

bool ContainsRecipient(std::string_view recipients, std::string_view character)
{
	if (recipients.empty())
		return false;

	while (!recipients.empty())
	{
		const size_t end = recipients.find('|');
		const std::string_view recipient = end == std::string_view::npos
			? recipients
			: recipients.substr(0, end);

		if (ci_equals(recipient, character))
			return true;

		if (end == std::string_view::npos)
			break;

		recipients.remove_prefix(end + 1);
	}

	return false;
}

int RecipientIndex(std::string_view recipients, std::string_view character)
{
	int index = 0;
	while (!recipients.empty())
	{
		const size_t end = recipients.find('|');
		const std::string_view recipient = end == std::string_view::npos
			? recipients
			: recipients.substr(0, end);

		if (ci_equals(recipient, character))
			return index;

		if (end == std::string_view::npos)
			break;

		recipients.remove_prefix(end + 1);
		++index;
	}

	return 0;
}

bool IsRecipientOrPeerMember(std::string_view recipients, std::string_view currentCharacter,
	std::string_view sender, Audience audience)
{
	if (ContainsRecipient(recipients, currentCharacter))
		return true;

	if (audience == Audience::Group)
		return IsCharacterInGroup(sender);

	if (audience == Audience::Raid)
		return IsCharacterInRaid(sender);

	return false;
}

std::string GroupRecipients()
{
	std::vector<std::string> recipients;
	AddRecipient(recipients, CurrentCharacter().c_str());

	if (pLocalPC && pLocalPC->Group)
	{
		for (uint32_t i = 0; i < pLocalPC->Group->GetMaxGroupSize(); ++i)
		{
			CGroupMember* member = pLocalPC->Group->GetGroupMember(i);
			if (member)
				AddRecipient(recipients, member->GetName());
		}
	}

	return JoinRecipients(recipients);
}

std::string RaidRecipients()
{
	std::vector<std::string> recipients;
	AddRecipient(recipients, CurrentCharacter().c_str());

	if (pRaid)
	{
		for (int i = 0; i < MAX_RAID_SIZE; ++i)
		{
			if (pRaid->RaidMemberUsed[i])
				AddRecipient(recipients, pRaid->RaidMember[i].Name);
		}
	}

	return JoinRecipients(recipients);
}

bool IsClass(std::string_view selector)
{
	if (!pLocalPlayer)
		return false;

	const int classId = pLocalPlayer->GetClass();
	if (classId <= 0 || classId > TotalPlayerClasses)
		return false;

	return ci_equals(selector, ClassInfo[classId].ShortName)
		|| ci_equals(selector, ClassInfo[classId].UCShortName)
		|| ci_equals(selector, ClassInfo[classId].Name);
}

bool IsRole(std::string_view selector)
{
	if (!pLocalPlayer)
		return false;

	const int classId = pLocalPlayer->GetClass();

	if (ci_equals(selector, "tank"))
		return classId == Warrior || classId == Paladin || classId == Shadowknight;

	if (ci_equals(selector, "priest") || ci_equals(selector, "healer"))
		return classId == Cleric || classId == Druid || classId == Shaman;

	if (ci_equals(selector, "caster"))
		return classId == Necromancer || classId == Wizard || classId == Magician || classId == Enchanter;

	if (ci_equals(selector, "melee"))
		return classId == Bard || classId == Ranger || classId == Monk
			|| classId == Rogue || classId == Beastlord || classId == Berserker;

	return false;
}

bool MatchesSelector(std::string_view selector, std::string_view sender)
{
	if (selector.empty() || ci_equals(selector, "all"))
		return true;

	if (ci_equals(selector, "zone"))
		return true;

	if (ci_equals(selector, "group"))
		return IsCharacterInGroup(sender);

	if (ci_equals(selector, "raid"))
		return IsCharacterInRaid(sender);

	return IsRole(selector) || IsClass(selector);
}

std::string Serialize(const NetCommand& command)
{
	return fmt::format("{}\n{}\n{}\n{}\n{}\n{}\n{}\n{}\n{}\n{}\n{}\n{}",
		PayloadHeader,
		static_cast<int>(command.Kind),
		static_cast<int>(command.AudienceType),
		command.IncludeSelf ? 1 : 0,
		command.ZoneOnly ? 1 : 0,
		command.DelayMs,
		command.StaggerMs,
		command.Id,
		command.Sender,
		command.Zone,
		command.Selector,
		command.Command);
}

bool ReadLine(std::string_view& text, std::string_view& line)
{
	const size_t end = text.find('\n');
	if (end == std::string_view::npos)
	{
		line = text;
		text = {};
		return true;
	}

	line = text.substr(0, end);
	text.remove_prefix(end + 1);
	return true;
}

bool Deserialize(std::string_view payload, NetCommand& command)
{
	std::array<std::string_view, 11> fields;
	for (std::string_view& field : fields)
	{
		if (!ReadLine(payload, field))
			return false;
	}

	if (fields[0] != PayloadHeader)
		return false;

	const int kind = GetIntFromString(std::string(fields[1]).c_str(), -1);
	if (kind < static_cast<int>(MessageKind::Execute) || kind > static_cast<int>(MessageKind::Presence))
		return false;

	const int audience = GetIntFromString(std::string(fields[2]).c_str(), -1);
	if (audience < static_cast<int>(Audience::All) || audience > static_cast<int>(Audience::Selector))
		return false;

	command.Kind = static_cast<MessageKind>(kind);
	command.AudienceType = static_cast<Audience>(audience);
	command.IncludeSelf = fields[3] == "1";
	command.ZoneOnly = fields[4] == "1";
	command.DelayMs = GetIntFromString(std::string(fields[5]).c_str(), 0);
	command.StaggerMs = GetIntFromString(std::string(fields[6]).c_str(), 0);
	command.Id = GetIntFromString(std::string(fields[7]).c_str(), 0);
	command.Sender = fields[8];
	command.Zone = fields[9];
	command.Selector = fields[10];
	command.Command = payload;
	trim(command.Command);

	return !command.Sender.empty() && !command.Command.empty();
}

bool ShouldReceive(const NetCommand& command)
{
	if (!command.IncludeSelf && IsSender(command.Sender))
		return false;

	if (command.ZoneOnly && !ci_equals(command.Zone, CurrentZone()))
		return false;

	const std::string currentCharacter = CurrentCharacter();

	if (command.AudienceType == Audience::Group
		&& !IsRecipientOrPeerMember(command.Selector, currentCharacter, command.Sender, command.AudienceType))
		return false;

	if (command.AudienceType == Audience::Raid
		&& !IsRecipientOrPeerMember(command.Selector, currentCharacter, command.Sender, command.AudienceType))
		return false;

	if (command.AudienceType == Audience::Selector && !MatchesSelector(command.Selector, command.Sender))
		return false;

	return true;
}

postoffice::Address BaseAddress()
{
	postoffice::Address address;
	address.Server = GetServerShortName();
	address.Mailbox = std::string(MailboxName);
	return address;
}

void SendToCharacter(std::string_view character, const NetCommand& command)
{
	postoffice::Address address = BaseAddress();
	address.Character = std::string(character);

	s_dropbox.Post(address, Serialize(command));
}

NetCommand MakeLocalMessage(MessageKind kind)
{
	NetCommand command;
	command.Kind = kind;
	command.IncludeSelf = false;
	command.Sender = CurrentCharacter();
	command.Zone = CurrentZone();
	command.Selector = CurrentClass();
	return command;
}

void SendStatusReply(std::string_view target)
{
	NetCommand reply = MakeLocalMessage(MessageKind::StatusReply);
	reply.Command = "status";
	SendToCharacter(target, reply);
}

std::string EvaluateExpression(std::string_view expression)
{
	char buffer[MAX_STRING] = { 0 };
	strcpy_s(buffer, std::string(expression).c_str());
	ParseMacroParameter(buffer, sizeof(buffer));
	return buffer;
}

void HandleQueryRequest(const NetCommand& command)
{
	if (!ShouldReceive(command))
		return;

	NetCommand reply = MakeLocalMessage(MessageKind::QueryReply);
	reply.Id = command.Id;
	reply.Command = EvaluateExpression(command.Command);
	SendToCharacter(command.Sender, reply);
}

void ExecuteCommandWithDelay(const NetCommand& command)
{
	int delay = command.DelayMs;
	if (command.StaggerMs > 0)
		delay += RecipientIndex(command.Selector, CurrentCharacter()) * command.StaggerMs;

	if (delay > 0)
		TimedCommand(command.Command.c_str(), delay);
	else
		DoCommand(command.Command.c_str(), true);
}

void ExecuteReceivedCommand(const std::shared_ptr<postoffice::Message>& message)
{
	if (!message || !message->Payload || !IsReady())
		return;

	NetCommand command;
	if (!Deserialize(*message->Payload, command))
		return;

	UpdatePeer(command.Sender, command.Zone, command.Selector);

	switch (command.Kind)
	{
	case MessageKind::Execute:
		if (ShouldReceive(command))
			ExecuteCommandWithDelay(command);
		break;

	case MessageKind::Chat:
		if (ShouldReceive(command))
			WriteChatf("\ag[AC]\ax %s: %s", command.Sender.c_str(), command.Command.c_str());
		break;

	case MessageKind::StatusRequest:
		if (!IsSender(command.Sender))
			SendStatusReply(command.Sender);
		break;

	case MessageKind::StatusReply:
		WriteChatf("\ag[AC]\ax %s zone=%s class=%s", command.Sender.c_str(), command.Zone.c_str(), command.Selector.c_str());
		break;

	case MessageKind::QueryRequest:
		HandleQueryRequest(command);
		break;

	case MessageKind::QueryReply:
		WriteChatf("\ag[AC]\ax query %u from %s: %s", command.Id, command.Sender.c_str(), command.Command.c_str());
		break;

	case MessageKind::Foreground:
		if (ShouldReceive(command))
			DoCommand("/foreground", true);
		break;

	case MessageKind::Presence:
		break;
	}
}

void PrintUsage()
{
	WriteChatf("\ayMQACNet\ax actor commands:");
	WriteChatf("  /ac <command>       Broadcast to all except yourself");
	WriteChatf("  /aca <command>      Broadcast to all except yourself");
	WriteChatf("  /acaa <command>     Broadcast to all including yourself");
	WriteChatf("  /actell <toon> <command>  Send to one character");
	WriteChatf("  /act <toon> <command>     Alias for /actell");
	WriteChatf("  /acg <command>      Broadcast to group except yourself");
	WriteChatf("  /acga <command>     Broadcast to group including yourself");
	WriteChatf("  /acgz <command>     Broadcast to group in zone except yourself");
	WriteChatf("  /acgza <command>    Broadcast to group in zone including yourself");
	WriteChatf("  /acr <command>      Broadcast to raid except yourself");
	WriteChatf("  /acra <command>     Broadcast to raid including yourself");
	WriteChatf("  /acrz <command>     Broadcast to raid in zone except yourself");
	WriteChatf("  /acrza <command>    Broadcast to raid in zone including yourself");
	WriteChatf("  /acza <command>     Broadcast to all in zone including yourself");
	WriteChatf("  /acge <selector> <command>   Execute by selector except yourself");
	WriteChatf("  /acgae <selector> <command>  Execute by selector including yourself");
	WriteChatf("  /acstatus          Show known peers and request live replies");
	WriteChatf("  /acmsg <selector> <message>  Send an AC chat message");
	WriteChatf("  /acquery <selector> <expr>   Ask peers to evaluate an expression");
	WriteChatf("  /acfg <selector>   Bring matching clients to foreground");
	WriteChatf("  Selectors: all, zone, group, raid, tank, priest, melee, caster, class name/shortname");
}

void Broadcast(Audience audience, bool includeSelf, bool zoneOnly, const char* line)
{
	int delayMs = 0;
	int staggerMs = 0;
	int consumedArgs = 0;
	char arg[MAX_STRING] = { 0 };

	while (true)
	{
		GetArg(arg, line, consumedArgs + 1);
		if (arg[0] == 0)
			break;

		if (!_stricmp(arg, "-delay") || !_stricmp(arg, "delay"))
		{
			char value[MAX_STRING] = { 0 };
			GetArg(value, line, consumedArgs + 2);
			if (value[0] == 0)
				break;

			delayMs = GetIntFromString(value, 0);
			consumedArgs += 2;
			continue;
		}

		if (!_stricmp(arg, "-stagger") || !_stricmp(arg, "stagger"))
		{
			char value[MAX_STRING] = { 0 };
			GetArg(value, line, consumedArgs + 2);
			if (value[0] == 0)
				break;

			staggerMs = GetIntFromString(value, 0);
			consumedArgs += 2;
			continue;
		}

		break;
	}

	const char* commandStart = consumedArgs > 0 ? GetNextArg(line, consumedArgs) : line;
	const std::string commandText = Trimmed(commandStart);
	if (commandText.empty())
	{
		PrintUsage();
		return;
	}

	if (!IsReady())
	{
		WriteChatf("\ayMQACNet:\ax You must be in game to send commands.");
		return;
	}

	NetCommand command;
	command.AudienceType = audience;
	command.IncludeSelf = includeSelf;
	command.ZoneOnly = zoneOnly;
	command.DelayMs = delayMs;
	command.StaggerMs = staggerMs;
	command.Sender = CurrentCharacter();
	command.Zone = CurrentZone();
	if (audience == Audience::Group)
		command.Selector = GroupRecipients();
	else if (audience == Audience::Raid)
		command.Selector = RaidRecipients();
	command.Command = commandText;

	s_dropbox.Post(BaseAddress(), Serialize(command));
}

void SendRouted(MessageKind kind, Audience audience, bool includeSelf, bool zoneOnly, const std::string& text,
	const std::string& selector = {})
{
	if (!IsReady())
	{
		WriteChatf("\ayMQACNet:\ax You must be in game to send commands.");
		return;
	}

	NetCommand command = MakeLocalMessage(kind);
	command.AudienceType = audience;
	command.IncludeSelf = includeSelf;
	command.ZoneOnly = zoneOnly;
	command.Command = text;
	command.Id = s_nextMessageId++;

	if (!selector.empty())
		command.Selector = selector;
	else if (audience == Audience::Group)
		command.Selector = GroupRecipients();
	else if (audience == Audience::Raid)
		command.Selector = RaidRecipients();

	s_dropbox.Post(BaseAddress(), Serialize(command));
}

void SelectorBroadcast(bool includeSelf, const char* line)
{
	char selector[MAX_STRING] = { 0 };
	GetArg(selector, line, 1);

	const char* commandStart = GetNextArg(line, 1);
	const std::string commandText = Trimmed(commandStart);

	if (selector[0] == 0 || commandText.empty())
	{
		PrintUsage();
		return;
	}

	if (!IsReady())
	{
		WriteChatf("\ayMQACNet:\ax You must be in game to send commands.");
		return;
	}

	NetCommand command;
	command.AudienceType = Audience::Selector;
	command.IncludeSelf = includeSelf;
	command.ZoneOnly = ci_equals(selector, "zone");
	command.Sender = CurrentCharacter();
	command.Zone = CurrentZone();
	command.Selector = selector;
	command.Command = commandText;

	s_dropbox.Post(BaseAddress(), Serialize(command));
}

void MessageCommand(const char* line)
{
	char selector[MAX_STRING] = { 0 };
	GetArg(selector, line, 1);

	const std::string text = Trimmed(GetNextArg(line, 1));
	if (selector[0] == 0 || text.empty())
	{
		WriteChatf("\ayUsage:\ax /acmsg <all|zone|group|raid|role|class> <message>");
		return;
	}

	const bool zoneOnly = ci_equals(selector, "zone");
	SendRouted(MessageKind::Chat, Audience::Selector, true, zoneOnly, text, selector);
}

void MessageToCommand(const char* line)
{
	char target[MAX_STRING] = { 0 };
	GetArg(target, line, 1);

	const std::string text = Trimmed(GetNextArg(line, 1));
	if (target[0] == 0 || text.empty())
	{
		WriteChatf("\ayUsage:\ax /acmsgto <toon> <message>");
		return;
	}

	NetCommand command = MakeLocalMessage(MessageKind::Chat);
	command.AudienceType = Audience::Target;
	command.IncludeSelf = true;
	command.Command = text;
	SendToCharacter(target, command);
}

void QueryCommand(const char* line)
{
	char selector[MAX_STRING] = { 0 };
	GetArg(selector, line, 1);

	const std::string expression = Trimmed(GetNextArg(line, 1));
	if (selector[0] == 0 || expression.empty())
	{
		WriteChatf("\ayUsage:\ax /acquery <all|zone|group|raid|role|class> <expression>");
		return;
	}

	const bool zoneOnly = ci_equals(selector, "zone");
	SendRouted(MessageKind::QueryRequest, Audience::Selector, false, zoneOnly, expression, selector);
}

void QueryToCommand(const char* line)
{
	char target[MAX_STRING] = { 0 };
	GetArg(target, line, 1);

	const std::string expression = Trimmed(GetNextArg(line, 1));
	if (target[0] == 0 || expression.empty())
	{
		WriteChatf("\ayUsage:\ax /acqueryto <toon> <expression>");
		return;
	}

	NetCommand command = MakeLocalMessage(MessageKind::QueryRequest);
	command.AudienceType = Audience::Target;
	command.IncludeSelf = true;
	command.Id = s_nextMessageId++;
	command.Command = expression;
	SendToCharacter(target, command);
}

void StatusCommand()
{
	WriteChatf("\ayMQACNet peers:\ax");
	const uint64_t now = NowMs();
	for (const auto& [_, peer] : s_peers)
	{
		WriteChatf("  %s zone=%s class=%s last=%llus",
			peer.Character.c_str(), peer.Zone.c_str(), peer.ClassName.c_str(),
			static_cast<unsigned long long>((now - peer.LastSeen) / 1000));
	}

	SendRouted(MessageKind::StatusRequest, Audience::All, false, false, "status");
}

void ForegroundCommand(const char* line)
{
	const std::string selector = Trimmed(line);
	if (selector.empty())
	{
		WriteChatf("\ayUsage:\ax /acfg <all|zone|group|raid|role|class>");
		return;
	}

	SendRouted(MessageKind::Foreground, Audience::Selector, false, ci_equals(selector, "zone"), "foreground", selector);
}

void ForegroundToCommand(const char* line)
{
	char target[MAX_STRING] = { 0 };
	GetArg(target, line, 1);
	if (target[0] == 0)
	{
		WriteChatf("\ayUsage:\ax /acfgto <toon>");
		return;
	}

	NetCommand command = MakeLocalMessage(MessageKind::Foreground);
	command.AudienceType = Audience::Target;
	command.IncludeSelf = true;
	command.Command = "foreground";
	SendToCharacter(target, command);
}

void Tell(const char* line)
{
	char target[MAX_STRING] = { 0 };
	GetArg(target, line, 1);

	const char* commandStart = GetNextArg(line, 1);
	const std::string commandText = Trimmed(commandStart);

	if (target[0] == 0 || commandText.empty())
	{
		PrintUsage();
		return;
	}

	if (!IsReady())
	{
		WriteChatf("\ayMQACNet:\ax You must be in game to send commands.");
		return;
	}

	NetCommand command;
	command.AudienceType = Audience::Target;
	command.IncludeSelf = true;
	command.Sender = CurrentCharacter();
	command.Zone = CurrentZone();
	command.Command = commandText;

	postoffice::Address address = BaseAddress();
	address.Character = target;

	s_dropbox.Post(address, Serialize(command));
}

void CmdAllButMe(PlayerClient*, const char* line) { Broadcast(Audience::All, false, false, line); }
void CmdAllIncludingMe(PlayerClient*, const char* line) { Broadcast(Audience::All, true, false, line); }
void CmdAllInZoneButMe(PlayerClient*, const char* line) { Broadcast(Audience::All, false, true, line); }
void CmdAllInZoneIncludingMe(PlayerClient*, const char* line) { Broadcast(Audience::All, true, true, line); }
void CmdGroupButMe(PlayerClient*, const char* line) { Broadcast(Audience::Group, false, false, line); }
void CmdGroupIncludingMe(PlayerClient*, const char* line) { Broadcast(Audience::Group, true, false, line); }
void CmdGroupInZoneButMe(PlayerClient*, const char* line) { Broadcast(Audience::Group, false, true, line); }
void CmdGroupInZoneIncludingMe(PlayerClient*, const char* line) { Broadcast(Audience::Group, true, true, line); }
void CmdRaidButMe(PlayerClient*, const char* line) { Broadcast(Audience::Raid, false, false, line); }
void CmdRaidIncludingMe(PlayerClient*, const char* line) { Broadcast(Audience::Raid, true, false, line); }
void CmdRaidInZoneButMe(PlayerClient*, const char* line) { Broadcast(Audience::Raid, false, true, line); }
void CmdRaidInZoneIncludingMe(PlayerClient*, const char* line) { Broadcast(Audience::Raid, true, true, line); }
void CmdSelectorButMe(PlayerClient*, const char* line) { SelectorBroadcast(false, line); }
void CmdSelectorIncludingMe(PlayerClient*, const char* line) { SelectorBroadcast(true, line); }
void CmdTell(PlayerClient*, const char* line) { Tell(line); }
void CmdMessage(PlayerClient*, const char* line) { MessageCommand(line); }
void CmdMessageTo(PlayerClient*, const char* line) { MessageToCommand(line); }
void CmdQuery(PlayerClient*, const char* line) { QueryCommand(line); }
void CmdQueryTo(PlayerClient*, const char* line) { QueryToCommand(line); }
void CmdStatus(PlayerClient*, const char*) { StatusCommand(); }
void CmdForeground(PlayerClient*, const char* line) { ForegroundCommand(line); }
void CmdForegroundTo(PlayerClient*, const char* line) { ForegroundToCommand(line); }

void CmdRoot(PlayerClient*, const char* line)
{
	const std::string commandText = Trimmed(line);
	if (commandText.empty() || ci_equals(commandText, "help"))
	{
		PrintUsage();
		return;
	}

	Broadcast(Audience::All, false, false, commandText.c_str());
}

constexpr std::array<std::string_view, 33> Commands = {
	"/ac",
	"/aca",
	"/acaa",
	"/actell",
	"/act",
	"/acexecute",
	"/acex",
	"/acg",
	"/acga",
	"/acgz",
	"/acgza",
	"/acgge",
	"/acgga",
	"/acr",
	"/acra",
	"/acrz",
	"/acrza",
	"/acre",
	"/acrea",
	"/acza",
	"/acze",
	"/aczea",
	"/acge",
	"/acgae",
	"/acmsg",
	"/acmsgto",
	"/acquery",
	"/acqueryto",
	"/acstatus",
	"/acwho",
	"/acping",
	"/acfg",
	"/acfgto",
};

} // namespace

PLUGIN_API void InitializePlugin()
{
	DebugSpewAlways("MQACNet::Initializing version %f", MQ2Version);

	s_dropbox = postoffice::AddActor(std::string(MailboxName).c_str(), ExecuteReceivedCommand);

	AddCommand("/ac", CmdRoot, false, true, true);
	AddCommand("/aca", CmdAllButMe, false, true, true);
	AddCommand("/acaa", CmdAllIncludingMe, false, true, true);
	AddCommand("/actell", CmdTell, false, true, true);
	AddCommand("/act", CmdTell, false, true, true);
	AddCommand("/acexecute", CmdTell, false, true, true);
	AddCommand("/acex", CmdTell, false, true, true);
	AddCommand("/acg", CmdGroupButMe, false, true, true);
	AddCommand("/acga", CmdGroupIncludingMe, false, true, true);
	AddCommand("/acgz", CmdGroupInZoneButMe, false, true, true);
	AddCommand("/acgza", CmdGroupInZoneIncludingMe, false, true, true);
	AddCommand("/acgge", CmdGroupButMe, false, true, true);
	AddCommand("/acgga", CmdGroupIncludingMe, false, true, true);
	AddCommand("/acr", CmdRaidButMe, false, true, true);
	AddCommand("/acra", CmdRaidIncludingMe, false, true, true);
	AddCommand("/acrz", CmdRaidInZoneButMe, false, true, true);
	AddCommand("/acrza", CmdRaidInZoneIncludingMe, false, true, true);
	AddCommand("/acre", CmdRaidButMe, false, true, true);
	AddCommand("/acrea", CmdRaidIncludingMe, false, true, true);
	AddCommand("/acza", CmdAllInZoneIncludingMe, false, true, true);
	AddCommand("/acze", CmdAllInZoneButMe, false, true, true);
	AddCommand("/aczea", CmdAllInZoneIncludingMe, false, true, true);
	AddCommand("/acge", CmdSelectorButMe, false, true, true);
	AddCommand("/acgae", CmdSelectorIncludingMe, false, true, true);
	AddCommand("/acmsg", CmdMessage, false, true, true);
	AddCommand("/acmsgto", CmdMessageTo, false, true, true);
	AddCommand("/acquery", CmdQuery, false, false, true);
	AddCommand("/acqueryto", CmdQueryTo, false, false, true);
	AddCommand("/acstatus", CmdStatus, false, true, true);
	AddCommand("/acwho", CmdStatus, false, true, true);
	AddCommand("/acping", CmdStatus, false, true, true);
	AddCommand("/acfg", CmdForeground, false, true, true);
	AddCommand("/acfgto", CmdForegroundTo, false, true, true);
}

PLUGIN_API void OnPulse()
{
	if (!IsReady() || NowMs() < s_nextPresence)
		return;

	s_nextPresence = NowMs() + 5000;

	NetCommand presence = MakeLocalMessage(MessageKind::Presence);
	presence.AudienceType = Audience::All;
	presence.IncludeSelf = false;
	presence.Command = "presence";

	s_dropbox.Post(BaseAddress(), Serialize(presence));
}

PLUGIN_API void ShutdownPlugin()
{
	DebugSpewAlways("MQACNet::Shutting down");

	for (std::string_view command : Commands)
		RemoveCommand(command);

	s_dropbox.Remove();
}
