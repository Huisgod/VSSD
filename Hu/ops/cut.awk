BEGIN{
i=0
}

{
i++
if(i%2==1)
print($2"\t"$10)
}

END{

}
