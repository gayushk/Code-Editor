static bool apply_command(const std::string& cmd, std::string& content) {
	if (cmd.size() < 3) return false;
	try {
		if (cmd[0] == 'I' && cmd[1] == ':') {
			size_t sep = cmd.find(':', 2);
			if (sep == std::string::npos) return false;
			int pos = std::stoi(cmd.substr(2, sep - 2));
			if (cmd.size() <= sep + 1) return false;
			char ch = cmd[sep + 1];
			if (pos < 0) pos = 0;
			if (pos > (int)content.size()) pos = (int)content.size();
			content.insert(pos, 1, ch);
			return true;
		}
		if (cmd[0] == 'D' && cmd[1] == ':') {
			int pos = std::stoi(cmd.substr(2));
			if (pos < 0 || pos >= (int)content.size()) return false;
			content.erase(pos, 1);
			return true;
		}
	} catch (const std::exception&) { return false; }
	return false;
}
