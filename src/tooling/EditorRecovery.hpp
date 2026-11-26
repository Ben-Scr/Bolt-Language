// Included inside Server. These read-only answers use current declarations and
// visible locals when the compiler has no binding for the requested token.
J editor_navigation(const std::string& file,const J& pos,bool hover)const {
    const auto source=text_for(file);
    const auto byte=offset(source,pos);
    const auto active=active_source(file,source);
    const EditorTokens scan(active.text);
    auto index=scan.at(byte);
    if(index>=scan.tokens.size()||scan.tokens[index].start!=byte){
        if(!index)return hover?J(nullptr):J(A{});
        --index;
    }
    const auto& token=scan.tokens[index];
    if(byte>=token.start+token.length||!token.kind.empty()||!EditorContext::is_name(token))
        return hover?J(nullptr):J(A{});
    const auto inspection=inspections.find(root_for(file));
    if(inspection!=inspections.end())for(const auto& diagnostic:inspection->second.diagnostics){
        const auto& at=diagnostic.location;
        if(at.file==file&&at.offset<=token.start&&token.start<at.offset+static_cast<std::size_t>(std::max(1,at.length))&&
           !diagnostic.message.starts_with("missing import "))return hover?J(nullptr):J(A{});
    }
    if(attribute_prefix(active.text,scan,token.start+token.length))return hover?J(nullptr):J(A{});
    EditorContext context(completion_program(file),scan,file,token.start+token.length);
    auto candidates=context.candidates(index);
    // For a complete call, syntax alone can exclude overloads with the wrong
    // arity, even if an unrelated error prevented semantic overload selection.
    std::optional<std::size_t> argument_count;
    auto open=index+1;
    if(open<scan.tokens.size()&&scan.tokens[open].text=="<"){
        int depth=0;
        for(;open<scan.tokens.size();++open){
            const auto& t=scan.tokens[open].text;
            if(t=="<")++depth;
            else if(t==">"&&!--depth){++open;break;}
            else if(t==";"||t=="("||t=="{")break;
        }
    }
    if(open<scan.tokens.size()&&scan.tokens[open].text=="("&&scan.pair[open]<scan.tokens.size()){
        const auto close=scan.pair[open];std::size_t count=open+1==close?0:1;
        bool simple=true;
        for(auto n=open+1;n<close;++n){
            if(scan.pair[n]>n&&scan.pair[n]<close)n=scan.pair[n];
            else if(scan.tokens[n].text=="<"){simple=false;break;} // Nested generic commas need semantic parsing.
            else if(scan.tokens[n].text==",")++count;
        }
        if(simple)argument_count=count;
    }
    const bool member=index&&(scan.tokens[index-1].text=="."||
        (index>=2&&scan.tokens[index-1].text==">"&&scan.tokens[index-2].text=="-"));
    // A qualified type name is a member of a namespace, not of a class.
    if(member&&std::none_of(candidates.begin(),candidates.end(),[&](const auto& item){return item.name==token.text;})){
        const auto receiver=context.receiver(index+1);
        if(receiver.stat){
            if(const auto* c=context.find_class(receiver.type);c&&context.accessible(c->name))candidates.push_back(EditorContext::class_item(*c));
            if(const auto* e=context.find_enum(receiver.type);e&&context.accessible(e->name))candidates.push_back(EditorContext::enum_item(*e));
        }
    }
    std::set<std::string> details;
    std::set<std::tuple<std::string,int,int>> targets;
    A definitions;
    std::string description;
    for(const auto& item:candidates){
        if(item.name!=token.text)continue;
        if(item.category=="method"&&argument_count&&(*argument_count<item.required_arguments||
            (!item.value_pack&&*argument_count>item.parameters.size())))continue;
        // Declaration-only recovery can expose multiple overloads. Return each
        // distinct declaration without guessing which call will type-check.
        if(!description.empty()&&item.category!="method")break;
        if(details.insert(item.detail).second){
            if(!description.empty())description+='\n';
            description+=item.detail;
        }
        const auto& at=item.declaration;
        if(!at.file.empty()&&(!at.file.starts_with("<")||at.file.starts_with("<bolt-stdlib>/"))&&
           targets.emplace(at.file,at.line,at.column).second)definitions.emplace_back(location(at));
        if(item.category!="method")break;
    }
    if(!hover)return definitions;
    if(description.empty())return nullptr;
    return O{{"contents",O{{"kind","markdown"},{"value","```bolt\n"+description+"\n```"}}},
             {"range",range_json(source,scan.location(file,index))}};
}

J import_actions(const std::string& file,const J& params)const {
    A actions;
    const auto inspection=inspections.find(root_for(file));
    if(inspection==inspections.end())return actions;
    const auto& only=params["context"]["only"].array();
    if(!only.empty()&&std::none_of(only.begin(),only.end(),[](const J& kind){return kind.string()=="quickfix";}))return actions;
    const auto source=text_for(file);
    const auto first=offset(source,params["range"]["start"]),last=offset(source,params["range"]["end"]);
    const auto newline=source.find('\n');
    const std::string ending=newline!=source.npos&&newline&&source[newline-1]=='\r'?"\r\n":"\n";
    const auto insert=pos_json(position(source,source.starts_with("\xef\xbb\xbf")?3:0));
    const auto uri=params["textDocument"]["uri"].string();
    std::set<std::string> seen;
    for(const auto& d:inspection->second.diagnostics){
        constexpr std::string_view prefix="missing import ";
        if(d.location.file!=file||!d.message.starts_with(prefix))continue;
        const auto& at=d.location;
        if(at.offset>last||at.offset+static_cast<std::size_t>(std::max(1,at.length))<first)continue;
        const auto module=d.message.substr(prefix.size());
        if(!is_builtin_import(module)||!seen.insert(module).second)continue;
        A edits{O{{"range",O{{"start",insert},{"end",insert}}},{"newText","import "+module+ending}}};
        J edit=O{{"changes",O{{uri,edits}}}};
        if(versioned_workspace_edits){
            auto doc=documents.find(file);
            J version=doc==documents.end()?J(nullptr):J(doc->second.version);
            edit=O{{"documentChanges",A{O{{"textDocument",O{{"uri",uri},{"version",version}}},{"edits",std::move(edits)}}}}};
        }
        actions.emplace_back(O{{"title","Add import "+module},{"kind","quickfix"},{"isPreferred",true},
                               {"diagnostics",A{diagnostic(d)}},{"edit",std::move(edit)}});
    }
    return actions;
}
