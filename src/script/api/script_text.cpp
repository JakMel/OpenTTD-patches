/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file script_text.cpp Implementation of ScriptText. */

#include "../../stdafx.h"
#include "../../string_func.h"
#include "../../strings_func.h"
#include "../../game/game_text.hpp"
#include "script_text.hpp"
#include "script_log.hpp"
#include "../script_fatalerror.hpp"
#include "../script_instance.hpp"
#include "script_log.hpp"
#include "../../table/control_codes.h"
#include "../../3rdparty/fmt/format.h"

#include "table/strings.h"

#include "../../safeguards.h"

RawText::RawText(const std::string &text) : text(text)
{
}


ScriptText::ScriptText(HSQUIRRELVM vm) :
	string(STR_NULL), param(), paramc(0)
{
	int nparam = sq_gettop(vm) - 1;
	if (nparam < 1) {
		throw sq_throwerror(vm, "You need to pass at least a StringID to the constructor");
	}

	/* First resolve the StringID. */
	SQInteger sqstring;
	if (SQ_FAILED(sq_getinteger(vm, 2, &sqstring))) {
		throw sq_throwerror(vm, "First argument must be a valid StringID");
	}
	this->string = sqstring;

	/* The rest of the parameters must be arguments. */
	for (int i = 0; i < nparam - 1; i++) {
		/* Push the parameter to the top of the stack. */
		sq_push(vm, i + 3);

		if (SQ_FAILED(this->_SetParam(i, vm))) {
			this->~ScriptText();
			throw sq_throwerror(vm, "Invalid parameter");
		}

		/* Pop the parameter again. */
		sq_pop(vm, 1);
	}
}

SQInteger ScriptText::_SetParam(int parameter, HSQUIRRELVM vm)
{
	if (parameter >= SCRIPT_TEXT_MAX_PARAMETERS) return SQ_ERROR;

	switch (sq_gettype(vm, -1)) {
		case OT_STRING: {
			const SQChar *value;
			sq_getstring(vm, -1, &value);

			this->param[parameter] = StrMakeValid(value);
			break;
		}

		case OT_INTEGER: {
			SQInteger value;
			sq_getinteger(vm, -1, &value);

			this->param[parameter] = value;
			break;
		}

		case OT_INSTANCE: {
			SQUserPointer real_instance = nullptr;
			HSQOBJECT instance;

			sq_getstackobj(vm, -1, &instance);

			/* Validate if it is a GSText instance */
			sq_pushroottable(vm);
			sq_pushstring(vm, "GSText", -1);
			sq_get(vm, -2);
			sq_pushobject(vm, instance);
			if (sq_instanceof(vm) != SQTrue) return SQ_ERROR;
			sq_pop(vm, 3);

			/* Get the 'real' instance of this class */
			sq_getinstanceup(vm, -1, &real_instance, nullptr);
			if (real_instance == nullptr) return SQ_ERROR;

			ScriptText *value = static_cast<ScriptText *>(real_instance);
			this->param[parameter] = ScriptTextRef(value);
			break;
		}

		default: return SQ_ERROR;
	}

	if (this->paramc <= parameter) this->paramc = parameter + 1;
	return 0;
}

SQInteger ScriptText::SetParam(HSQUIRRELVM vm)
{
	if (sq_gettype(vm, 2) != OT_INTEGER) return SQ_ERROR;

	SQInteger k;
	sq_getinteger(vm, 2, &k);

	if (k > SCRIPT_TEXT_MAX_PARAMETERS) return SQ_ERROR;
	if (k < 1) return SQ_ERROR;
	k--;

	return this->_SetParam(k, vm);
}

SQInteger ScriptText::AddParam(HSQUIRRELVM vm)
{
	SQInteger res;
	res = this->_SetParam(this->paramc, vm);
	if (res != 0) return res;

	/* Push our own instance back on top of the stack */
	sq_push(vm, 1);
	return 1;
}

SQInteger ScriptText::_set(HSQUIRRELVM vm)
{
	int32 k;

	if (sq_gettype(vm, 2) == OT_STRING) {
		const SQChar *key_string;
		sq_getstring(vm, 2, &key_string);
		StrMakeValidInPlace(const_cast<char *>(key_string));

		if (strncmp(key_string, "param_", 6) != 0 || strlen(key_string) > 8) return SQ_ERROR;
		k = atoi(key_string + 6);
	} else if (sq_gettype(vm, 2) == OT_INTEGER) {
		SQInteger key;
		sq_getinteger(vm, 2, &key);
		k = (int32)key;
	} else {
		return SQ_ERROR;
	}

	if (k > SCRIPT_TEXT_MAX_PARAMETERS) return SQ_ERROR;
	if (k < 1) return SQ_ERROR;
	k--;

	return this->_SetParam(k, vm);
}

const std::string ScriptText::GetEncodedText()
{
	static char buf[1024];
	static StringIDList seen_ids;
	int param_count = 0;
	seen_ids.clear();
	this->_GetEncodedText(buf, lastof(buf), param_count, seen_ids);
	if (param_count > SCRIPT_TEXT_MAX_PARAMETERS) throw Script_FatalError(fmt::format("{}: Too many parameters", GetGameStringName(this->string)));
	return buf;
}

void ScriptText::_TextParamError(std::string msg)
{
	if (this->GetActiveInstance()->IsTextParamMismatchAllowed()) {
		ScriptLog::LogOnce(ScriptLogTypes::LOG_ERROR, std::move(msg));
	} else {
		throw Script_FatalError(std::move(msg));
	}
}

char *ScriptText::_GetEncodedText(char *p, char *lastofp, int &param_count, StringIDList &seen_ids)
{
	const std::string &name = GetGameStringName(this->string);

	if (std::find(seen_ids.begin(), seen_ids.end(), this->string) != seen_ids.end()) throw Script_FatalError(fmt::format("{}: Circular reference detected", name));
	seen_ids.push_back(this->string);

	p += Utf8Encode(p, SCC_ENCODED);
	p += seprintf(p, lastofp, "%X", this->string);

	auto write_param_fallback = [&](int idx) {
		if (std::holds_alternative<ScriptTextRef>(this->param[idx])) {
			int count = 1; // 1 because the string id is included in consumed parameters
			p += seprintf(p, lastofp, ":");
			p = std::get<ScriptTextRef>(this->param[idx])->_GetEncodedText(p, lastofp, count, seen_ids);
			param_count += count;
		} else if (std::holds_alternative<SQInteger>(this->param[idx])) {
			p += seprintf(p, lastofp, ":" OTTD_PRINTFHEX64, std::get<SQInteger>(this->param[idx]));
			param_count++;
		} else {
			/* Fallback value */
			p += seprintf(p, lastofp, ":0");
			param_count++;
		}
	};

	const StringParams &params = GetGameStringParams(this->string);
	int cur_idx = 0;
	int prev_string = -1;
	int prev_idx = -1;
	int prev_count = -1;

	for (const StringParam &cur_param : params) {
		if (cur_idx >= this->paramc) {
			this->_TextParamError(fmt::format("{}: Not enough parameters", name));
			break;
		}

		if (this->GetActiveInstance()->IsTextParamMismatchAllowed()) {
			switch (cur_param.type) {
				case StringParam::RAW_STRING:
					if (!std::holds_alternative<std::string>(this->param[cur_idx])) {
						this->_TextParamError(fmt::format("{}: Parameter {} expects a raw string", name, cur_idx));
						write_param_fallback(cur_idx++);
						break;
					}
					p += seprintf(p, lastofp, ":\"%s\"", std::get<std::string>(this->param[cur_idx++]).c_str());
					param_count++;
					break;

				case StringParam::STRING: {
					if (!std::holds_alternative<ScriptTextRef>(this->param[cur_idx])) {
						this->_TextParamError(fmt::format("{}: Parameter {} expects a substring", name, cur_idx));
						write_param_fallback(cur_idx++);
						break;
					}
					int count = 1; // 1 because the string id is included in consumed parameters
					p += seprintf(p, lastofp, ":");
					p = std::get<ScriptTextRef>(this->param[cur_idx++])->_GetEncodedText(p, lastofp, count, seen_ids);
					if (count != cur_param.consumes) {
						this->_TextParamError(fmt::format("{}: Parameter {} substring consumes {}, but expected {} to be consumed", name, cur_idx, count - 1, cur_param.consumes - 1));
					}
					param_count += count;
					break;
				}

				default:
					if (cur_idx + cur_param.consumes > this->paramc) {
						this->_TextParamError(fmt::format("{}: Not enough parameters", name));
					}
					for (int i = 0; i < cur_param.consumes && cur_idx < this->paramc; i++) {
						if (!std::holds_alternative<SQInteger>(this->param[cur_idx])) {
							this->_TextParamError(fmt::format("{}: Parameter {} expects an integer", name, cur_idx));
							write_param_fallback(cur_idx++);
							continue;
						}
						p += seprintf(p, lastofp, ":" OTTD_PRINTFHEX64, std::get<SQInteger>(this->param[cur_idx++]));
						param_count++;
					}
					break;
			}
		} else {
			if (prev_string != -1) {
				/* The previous substring added more parameters than expected, means we will consume them but can't properly validate them. */
				for (int i = 0; i < cur_param.consumes; i++) {
					if (prev_idx < prev_count) {
						ScriptLog::Warning(fmt::format("{}: Parameter {} uses parameter {} from substring {} and cannot be validated", name, param_count + i, prev_idx++, prev_string).c_str());
					} else {
						/* No more extra parameters, assume SQInteger are expected. */
						if (cur_idx >= this->paramc) throw Script_FatalError(fmt::format("{}: Not enough parameters", name));
						if (!std::holds_alternative<SQInteger>(this->param[cur_idx])) throw Script_FatalError(fmt::format("{}: Parameter {} expects an integer", name, param_count + i));
						p += seprintf(p, lastofp, ":" OTTD_PRINTFHEX64, std::get<SQInteger>(this->param[cur_idx++]));
					}
				}
				if (prev_idx == prev_count) {
					/* Re-enable validation. */
					prev_string = -1;
				}
			} else {
				switch (cur_param.type) {
					case StringParam::RAW_STRING:
						if (!std::holds_alternative<std::string>(this->param[cur_idx])) throw Script_FatalError(fmt::format("{}: Parameter {} expects a raw string", name, param_count));
						p += seprintf(p, lastofp, ":\"%s\"", std::get<std::string>(this->param[cur_idx++]).c_str());
						break;

					case StringParam::STRING: {
						if (!std::holds_alternative<ScriptTextRef>(this->param[cur_idx])) throw Script_FatalError(fmt::format("{}: Parameter {} expects a substring", name, param_count));
						int count = 0;
						p += seprintf(p, lastofp, ":");
						p = std::get<ScriptTextRef>(this->param[cur_idx++])->_GetEncodedText(p, lastofp, count, seen_ids);
						if (++count != cur_param.consumes) {
							ScriptLog::Error(fmt::format("{}: Parameter {} substring consumes {}, but expected {} to be consumed", name, param_count, count - 1, cur_param.consumes - 1).c_str());
							/* Fill missing params if needed. */
							for (int i = count; i < cur_param.consumes; i++) p += seprintf(p, lastofp, ":0");
							/* Disable validation for the extra params if any. */
							if (count > cur_param.consumes) {
								prev_string = param_count;
								prev_idx = cur_param.consumes - 1;
								prev_count = count - 1;
							}
						}
						break;
					}

					default:
						if (cur_idx + cur_param.consumes > this->paramc) throw Script_FatalError(fmt::format("{}: Not enough parameters", name));
						for (int i = 0; i < cur_param.consumes; i++) {
							if (!std::holds_alternative<SQInteger>(this->param[cur_idx])) throw Script_FatalError(fmt::format("{}: Parameter {} expects an integer", name, param_count + i));
							p += seprintf(p, lastofp, ":" OTTD_PRINTFHEX64, std::get<SQInteger>(this->param[cur_idx++]));
						}
				}
			}
		}
	}

	if (this->GetActiveInstance()->IsTextParamMismatchAllowed()) {
		for (int i = cur_idx; i < this->paramc; i++) {
			write_param_fallback(i);
		}
	}

	seen_ids.pop_back();

	return p;
}

const std::string Text::GetDecodedText()
{
	const std::string &encoded_text = this->GetEncodedText();

	static char buf[1024];
	::SetDParamStr(0, encoded_text);
	::GetString(buf, STR_JUST_RAW_STRING, lastof(buf));
	return buf;
}
