// Only a tie between semantically viable overloads produces navigation choices.
    J unresolved_method_group(const std::string& file,const J& position_)const {
        const auto found=indexes.find(root_for(file));if(found==indexes.end()||found->second.ambiguous.empty())return nullptr;
        const auto* occurrence=at_position(file,position_);if(!occurrence)return nullptr;
        const auto& index=found->second;
        auto group=index.ambiguous.find({file,occurrence->at.line,occurrence->at.column});if(group==index.ambiguous.end())return nullptr;
        A labels,targets;std::set<std::tuple<std::string,int,int>> seen;
        for(const auto& id:group->second)if(auto symbol=index.symbols.find(id);symbol!=index.symbols.end()) {
            const auto& target=symbol->second;
            if(!seen.emplace(target.at.file,target.at.line,target.at.column).second)continue;
            labels.emplace_back(target.type);targets.emplace_back(location(target.at));
        }
        if(targets.empty())return nullptr;
        const auto range=range_json(text_for(file),occurrence->at);
        const auto& first=index.symbols.at(group->second.front());
        auto hover=J(O{{"contents",O{{"kind","markdown"},{"value","```bolt\n"+labels.front().string()+"\n```\n\nAmbiguous call: equally applicable overloads."}}},{"range",range},
            {"bolt",O{{"label",labels.front()},{"overloads",labels},{"overloadDefinitions",targets},{"owner","Ambiguous overloads"},{"definition",targets.front()}}}});
        return O{{"name",first.name},{"category","method"},{"hover",hover},{"definition",targets.front()},{"definitions",targets},{"readOnly",true},{"ambiguous",true},{"range",range}};
    }
// Snapshot of resolved physical references for latency-free IDE hover/navigation.
    J symbol_index(const std::string& file)const {
        const auto found=indexes.find(root_for(file));if(found==indexes.end())return O{};
        const auto& index=found->second;const auto& doc=location_file(file);A occurrences;O symbols,virtuals;
        std::set<std::pair<int,int>> positions;
        for(const auto& hit:index.occurrences)if(hit.at.file==file&&!positions.contains({hit.at.line,hit.at.column})) {
            const auto resolved=index.symbols.find(hit.symbol);
            if(resolved==index.symbols.end())continue;
            const auto& target=resolved->second;
            if(target.name.starts_with("@")||target.owner.starts_with("Bolt.Generated."))continue;
            positions.emplace(hit.at.line,hit.at.column);
            occurrences.emplace_back(O{{"range",doc.lines.range(doc.text,hit.at)},{"symbol",hit.symbol}});
            if(symbols.contains(hit.symbol))continue;
            auto hover=hover_symbol(file,hit,target,index);
            symbols[hit.symbol]=O{{"name",target.name},{"category",target.category},{"hover",hover},
                {"definition",location(target.at)},{"readOnly",target.at.file.starts_with("<")}};
            if(target.at.file.starts_with("<bolt-stdlib>/"))virtuals[location(target.at)["uri"].string()]=text_for(target.at.file);
            for(const auto& definition:hover["bolt"]["overloadDefinitions"].array())if(definition["uri"].string().starts_with("bolt-stdlib:///")) {
                const auto native="<bolt-stdlib>/"+definition["uri"].string().substr(std::string("bolt-stdlib:///").size());
                virtuals[definition["uri"].string()]=text_for(native);
            }
        }
        for(const auto& [key,ids]:index.ambiguous) {
            const auto& [source_file,line,column]=key;if(source_file!=file)continue;
            auto range=doc.lines.range(doc.text,Location{file,line,column});
            auto group=unresolved_method_group(file,range["start"]);if(group.is_null())continue;
            const auto identity="group:"+std::to_string(line)+":"+std::to_string(column);symbols[identity]=group;
            for(auto& occurrence:occurrences)if(occurrence["range"]["start"].dump()==range["start"].dump())occurrence["symbol"]=identity;
            for(const auto& target:group["definitions"].array())if(target["uri"].string().starts_with("bolt-stdlib:///")) {
                const auto native="<bolt-stdlib>/"+target["uri"].string().substr(std::string("bolt-stdlib:///").size());
                virtuals[target["uri"].string()]=text_for(native);
            }
        }
        return O{{"occurrences",occurrences},{"symbols",symbols},{"virtualDocuments",virtuals},{"textHash",hash_text(doc.text)}};
    }
// Included inside Server: all operations use its current compiler-resolved snapshot.
    static bool identifier_name(const std::string& name) {
        if(name.empty()||!alpha(name[0])||keywords.contains(name)||primitives.contains(name))return false;
        return std::all_of(name.begin()+1,name.end(),[](char c){return alpha(c)||digit(c);});
    }
    J generic_context(const std::string& file,const J& pos)const {
        auto source=text_for(file);auto caret=offset(source,pos);auto active=active_source(file,source);EditorTokens scan(active.text);
        if(caret&&active.text[caret-1]==' '&&source[caret-1]!=' ')return O{{"generic",false}};
        auto end=scan.at(caret);if(end&&scan.tokens[end-1].text=="<")--end;
        if(!end)return O{{"generic",false}};
        auto name=end-1;const auto& word=scan.tokens[name].text;
        if(word.empty()||!alpha(word[0]))return O{{"generic",false}};
        EditorContext context(completion_program(file),scan,file,scan.tokens[name].start);
        // A real local with that name shadows a type/method.
        for(const auto& item:context.candidates(name))if(item.name==word) {
            if(item.category=="variable"||item.category=="parameter"||item.category=="field"||item.category=="property")return O{{"generic",false}};
            if(!item.type_parameters.empty()||!item.type_pack.empty())return O{{"generic",true}};
            if(item.category=="class"||item.category=="interface"||item.category=="struct")
                if(const auto* c=context.find_class(item.type);c&&(!c->generic_parameters.empty()||!c->pack_parameter.empty()))return O{{"generic",true}};
        }
        // New generic declarations can be recognized without inventing a referenced
        // symbol: the syntax is a declaration, never an expression comparison.
        if(name&&std::set<std::string>{"class","struct","interface","concept"}.contains(scan.tokens[name-1].text))return O{{"generic",true}};
        if(name&&context.owner&&!context.method) {
            const auto& previous=scan.tokens[name-1].text;
            if(primitives.contains(previous)||previous=="void"||std::find(context.owner->generic_parameters.begin(),context.owner->generic_parameters.end(),previous)!=context.owner->generic_parameters.end())return O{{"generic",true}};
        }
        return O{{"generic",false}};
    }
    std::set<std::string> symbol_family(const Index& index,const Symbol& symbol,const std::string& file)const {
        std::set<std::string> result{symbol.id};
        const auto* p=completion_program(file);
        for(const auto& [id,candidate]:index.symbols) {
            if(candidate.at.file==symbol.at.file&&candidate.at.line==symbol.at.line&&candidate.at.column==symbol.at.column)result.insert(id);
            if(symbol.category=="class"||symbol.category=="struct"||symbol.category=="interface") {
                if(candidate.owner==symbol.type&&(candidate.kind==9||candidate.name=="~"+symbol.name))result.insert(id);
            }
            if(p&&symbol.category=="method"&&candidate.category=="method"&&candidate.name==symbol.name&&!symbol.owner.empty()&&!candidate.owner.empty()) {
                const auto* a=p->find_class(symbol.owner);const auto* b=p->find_class(candidate.owner);
                if(a&&b&&(derives_from(*p,a->name,b->name)||derives_from(*p,b->name,a->name))) {
                    auto suffix=[](const std::string& id){return id.substr(id.find('('));};
                    if(id.find('(')!=id.npos&&symbol.id.find('(')!=symbol.id.npos&&suffix(id)==suffix(symbol.id))result.insert(id);
                }
            }
        }
        return result;
    }
    // A failed analysis retains a partial AST. Never report a layout before
    // the compiler has resolved every inline field and base type it depends on.
    static bool hover_layout_ready(const Program& program,std::string type,std::set<std::string>& visited) {
        type=referred(type);
        if(is_pointer(type)||is_array(type)||is_collection(type)||is_primitive(type)||type=="Arena")return true;
        if(!visited.insert(type).second)return true; // ValueLayouts rejects inline cycles.
        if(visited.size()>128)return false;
        if(is_optional(type))return hover_layout_ready(program,element_type(type),visited);
        if(const auto* e=program.find_enum(type))return !e->is_template_member&&is_integer(e->underlying);
        const auto* c=program.find_class(type);
        if(!c||!c->prepared||c->is_static||c->is_global||c->is_template_member||!c->generic_parameters.empty()||!c->pack_parameter.empty())return false;
        if(c->is_native||c->is_interface)return true;
        if(!c->base.empty()&&!hover_layout_ready(program,c->base,visited))return false;
        for(const auto& field:c->fields)if(!field.is_static&&!hover_layout_ready(program,field.type,visited))return false;
        return true;
    }
    J hover_size(const std::string& file,const Symbol& symbol)const {
        const auto& category=symbol.category;
        if(category!="class"&&category!="struct"&&category!="interface"&&category!="enum"&&category!="enumMember"&&
           category!="variable"&&category!="parameter"&&category!="field"&&category!="property")return O{};
        if(symbol.kind==14)return O{}; // Generic constant signatures are not value types.
        auto found=inspections.find(root_for(file));
        const auto* program=found==inspections.end()?nullptr:found->second.program.get();
        const auto type=referred(symbol.type);
        if(program)if(const auto* c=program->find_class(type)) {
            if(c->is_static||c->is_global)return O{};
            if(c->is_template_member||!c->generic_parameters.empty()||!c->pack_parameter.empty())
                return O{{"sizeNote","depends on type arguments"}};
        }
        std::set<std::string> visited;
        if(program&&hover_layout_ready(*program,type,visited))try {
            const auto bytes=native_abi::value_size(*program,type);
            if(bytes<=static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
                O size{{"sizeBytes",static_cast<std::int64_t>(bytes)}};
                if(is_reference(symbol.type))size["sizeNote"]="referenced value; excludes heap allocations";
                else if(is_pointer(type))size["sizeNote"]="pointer; excludes pointed-to data";
                else if(!is_numeric(type)&&type!="bool"&&type!="char"&&!program->find_enum(type))
                    size["sizeNote"]="excludes heap allocations";
                return size;
            }
        }catch(const std::exception&) { /* Unresolved or unsupported layouts have no numeric size. */ }
        return O{{"sizeNote","unavailable until the type layout is resolved"}};
    }
    J hover_symbol(const std::string& file,const Occurrence& occurrence,const Symbol& symbol,const Index& index)const {
        std::string label=!occurrence.detail.empty()?occurrence.detail:(symbol.category=="method"||symbol.kind==14?symbol.type:symbol.type+" "+symbol.name);
        A overloads,definitions;std::set<std::string> seen;
        if(symbol.category=="method")for(const auto& [id,item]:index.symbols)if(item.category=="method"&&item.owner==symbol.owner&&item.name==symbol.name&&seen.insert(item.type).second) {
            overloads.emplace_back(item.type);definitions.emplace_back(location(item.at));
        }
        if(overloads.empty()){overloads.emplace_back(label);definitions.emplace_back(location(symbol.at));}
        const auto size=hover_size(file,symbol);auto details=size;
        details["label"]=label;details["overloads"]=overloads;details["overloadDefinitions"]=definitions;
        details["owner"]=symbol.owner;details["definition"]=location(symbol.at);
        auto text="```bolt\n"+label+"\n```";
        if(size["sizeBytes"].is_integer()) {
            const auto bytes=size["sizeBytes"].integer();
            text+="\n\nSize: "+std::to_string(bytes)+(bytes==1?" byte":" bytes");
            if(!size["sizeNote"].string().empty())text+=" ("+size["sizeNote"].string()+")";
        }else if(!size["sizeNote"].string().empty())text+="\n\nSize: "+size["sizeNote"].string();
        if(!symbol.owner.empty())text+="\n\n"+symbol.owner;
        if(!symbol.at.file.empty()){
            auto doc=documents.find(symbol.at.file);
            const auto& name=doc!=documents.end()&&doc->second.uri.starts_with("untitled:")?doc->second.uri:symbol.at.file;
            text+="\n\n"+name+":"+std::to_string(symbol.at.line);
        }
        return O{{"contents",O{{"kind","markdown"},{"value",text}}},{"range",range_json(text_for(file),occurrence.at)},
            {"bolt",details}};
    }
    J prepare_rename(const std::string& file,const J& pos)const {
        auto it=indexes.find(root_for(file));auto o=at_position(file,pos);
        if(it==indexes.end()||!o||it->second.ambiguous.contains({file,o->at.line,o->at.column}))return nullptr;
        auto symbol=it->second.symbols.find(o->symbol);if(symbol==it->second.symbols.end())return nullptr;
        const auto& s=symbol->second;
        if(s.at.file.empty()||s.at.file[0]=='<'||!identifier_name(s.name)||s.kind==9)return nullptr;
        if(s.category=="method")if(const auto* p=completion_program(file);p)if(const auto* c=p->find_class(s.owner);c)for(const auto& m:c->methods)if(m.name==s.name&&(m.is_conversion||!m.operator_symbol.empty()))return nullptr;
        return O{{"range",range_json(text_for(file),o->at)},{"placeholder",s.name}};
    }
    J rename_symbol(const std::string& file,const J& pos,const std::string& new_name)const {
        if(!identifier_name(new_name))throw std::invalid_argument("Rename requires a non-keyword Bolt identifier");
        if(prepare_rename(file,pos).is_null())throw std::invalid_argument("This symbol is unresolved, compiler-owned, or cannot be renamed here");
        const auto root=root_for(file);const auto& inspection=inspections.at(root);
        if(!inspection.program||std::any_of(inspection.diagnostics.begin(),inspection.diagnostics.end(),[](const Diagnostic& d){return d.severity==Severity::Error;}))
            throw std::invalid_argument("Fix project errors before renaming; Bolt will not guess unresolved usages");
        const auto& index=indexes.at(root);const auto* hit=at_position(file,pos);const auto& symbol=index.symbols.at(hit->symbol);
        auto family=symbol_family(index,symbol,file);
        for(const auto& id:family)if(const auto& s=index.symbols.at(id);!s.at.file.empty()&&s.at.file[0]=='<')throw std::invalid_argument("Rename would change a compiler/standard-library contract");
        struct Edit {std::size_t start,end;J range;};
        std::map<std::string,std::vector<Edit>> edits;
        for(const auto& occurrence:index.occurrences)if(family.contains(occurrence.symbol)&&!occurrence.at.file.empty()&&occurrence.at.file[0]!='<') {
            const auto& doc=location_file(occurrence.at.file);const auto range=doc.lines.range(doc.text,occurrence.at);
            auto [start,end]=doc.lines.bounds(doc.text,occurrence.at);
            if(doc.text.compare(start,end-start,symbol.name)!=0)continue; // never rename synthetic lowered tokens
            auto& list=edits[occurrence.at.file];if(std::none_of(list.begin(),list.end(),[&](const Edit& e){return e.start==start;}))list.push_back({start,end,range});
        }
        if(edits.empty())throw std::invalid_argument("No editable references for this symbol");
        SourceOverrides overrides;for(const auto& [name,doc]:documents)overrides[name]=doc.text;
        for(auto& [name,list]:edits) {
            auto text=text_for(name);std::sort(list.begin(),list.end(),[](const Edit& a,const Edit& b){return a.start>b.start;});
            for(const auto& edit:list)text.replace(edit.start,edit.end-edit.start,new_name);
            overrides[name]=std::move(text);
        }
        auto candidate=inspect_project(path_from_utf8(root),overrides,false,session_preprocessing);
        if(!candidate.program)throw std::invalid_argument("Rename would make the project invalid");
        for(const auto& d:candidate.diagnostics)if(d.severity==Severity::Error)throw std::invalid_argument("Rename rejected: "+d.message);
        Index updated;updated.build(*candidate.program);
        auto moved=[&](const std::string& name,std::size_t old){std::ptrdiff_t delta=0;if(auto list=edits.find(name);list!=edits.end())for(const auto& edit:list->second)if(edit.start<old)delta+=static_cast<std::ptrdiff_t>(new_name.size())-static_cast<std::ptrdiff_t>(edit.end-edit.start);return static_cast<std::size_t>(static_cast<std::ptrdiff_t>(old)+delta);};
        auto declaration_key=[&](const Location& at,bool remap){const auto& doc=location_file(at.file);auto byte=doc.lines.bounds(doc.text,at).first;return at.file+":"+std::to_string(remap?moved(at.file,byte):byte);};
        std::map<std::string,SourceLines> candidate_lines;
        // A rename must not silently capture a different variable/overload. Check
        // all previously resolved physical references against their declaration.
        for(const auto& before:index.occurrences) {
            if(before.at.file.empty()||before.at.file[0]=='<')continue;
            auto old_symbol=index.symbols.find(before.symbol);if(old_symbol==index.symbols.end())continue;
            if(old_symbol->second.at.file.empty()||old_symbol->second.at.file[0]=='<')continue;
            const auto& old=location_file(before.at.file);
            auto [old_offset,end]=old.lines.bounds(old.text,before.at);
            if(old.text.compare(old_offset,end-old_offset,old_symbol->second.name)!=0)continue;
            const auto updated_text=candidate.sources.find(before.at.file);if(updated_text==candidate.sources.end())continue;
            auto expected_offset=moved(before.at.file,old_offset);
            bool resolved=false;
            const auto target_key=declaration_key(old_symbol->second.at,true);
            // Identifier renames cannot add/remove line breaks.
            const auto& fresh=updated_text->second;
            const auto& lines=candidate_lines.try_emplace(before.at.file,fresh).first->second;
            for(auto it=updated.line_begin(before.at.file,before.at.line);it!=updated.occurrences.end();++it) {
                const auto& after=*it;
                if(after.at.file!=before.at.file||after.at.line!=before.at.line)break;
                if(lines.bounds(fresh,after.at).first!=expected_offset)continue;
                auto new_symbol=updated.symbols.find(after.symbol);if(new_symbol==updated.symbols.end())continue;
                const auto& decl=new_symbol->second.at;auto declaration_source=candidate.sources.find(decl.file);if(declaration_source==candidate.sources.end())continue;
                const auto& decl_lines=candidate_lines.try_emplace(decl.file,declaration_source->second).first->second;
                auto declaration_offset=decl_lines.bounds(declaration_source->second,decl).first;
                if(decl.file+":"+std::to_string(declaration_offset)==target_key){resolved=true;break;}
            }
            if(!resolved)throw std::invalid_argument("Rename would alter name binding near "+before.at.file+":"+std::to_string(before.at.line));
        }
        A changes;
        for(const auto& [name,list]:edits) {
            A items;for(const auto& edit:list)items.emplace_back(O{{"range",edit.range},{"newText",new_name}});
            J version=nullptr;if(auto document=documents.find(name);document!=documents.end())version=document->second.version;
            changes.emplace_back(O{{"textDocument",O{{"uri",document_uri(name)},{"version",version}}},{"edits",items},{"boltOriginalText",text_for(name)}});
        }
        return O{{"documentChanges",changes}};
    }
